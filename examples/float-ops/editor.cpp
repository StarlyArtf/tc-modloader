/* The property rows of the float types, declared instead of drawn.

   Before this file the Mod owned a window over the game's component drawer and
   drew the rows itself (label, value, rounding radios, display mode, a hint
   line): that is the recipe docs/HANDOFF-component-editor.md §1 counted in
   every Mod, and it is why tc.component.editor exists.  Now each type hands the
   loader one field table - the same shape examples/editor-demo shows - and the
   host draws the rows over the drawer, validates every edit against the
   registered configuration and commits it through the same path
   tc.component.storage writes use (one commit = one undo step, and the type's
   on_config_changed fires exactly as before).

   What a type declares:

     Constant      值 (float over the binary32 bit pattern at +4)
                   原始位 (hex over the same four bytes - the escape hatch for
                   NaN/Inf/denormal patterns a float box cannot type)
                   标签 (16-byte text at +8)
     Constant 16/64 值 (the loader's width-aware decimal row over their own two
                   or eight bytes) + 原始位 (the same bytes as hex)
                   标签 (16-byte text at +12)
     Add           舍入模式 (5-way radio over the rounding byte at +1)
                   标签 (16-byte text at +4)
     Display       显示方式 (4-way radio over the mode byte at +1)
                   标签 (16-byte text at +4)
     the rest      舍入模式 (only where the type rounds) + 标签

   The catalogue types that carry no rounding choice used to show a hint line
   instead; that sentence now lives in the type's own description, which the
   game's drawer already prints under the title - one surface, not two.

   Degrading: an older loader has no tc.component.editor (or only its V1), and
   then the components keep working with their stored configuration and no rows
   appear; the flags this file needs from V2 (radio enums, the editable
   hexadecimal box) are only set when V2 answered, because a V1 loader refuses
   an unknown flag and would reject the whole table. */

#include "components_internal.hpp"

#include "../../sdk/tc_component_editor.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace floatops {
namespace internals {

namespace {

using tc::component_editor::Api;
using tc::component_editor::ApiV2;
using tc::component_editor::Field;

/* The stored byte is the enum value itself (fp/fp32.hpp: nearest_even = 0 …
   toward_positive = 4; fp/decimal.hpp: decimal_and_hex = 0 … hex_only = 3), so
   the label order below is the order the player sees and the order on disk. */
const char* const kRoundingLabels[] = {"RNE", "RNA", "RTZ", "RDN", "RUP"};
const char* const kDisplayLabels[] = {"decimal + hex", "shortest", "scientific",
                                      "hex only"};

constexpr uint32_t kRoundingChoices = 5;
constexpr uint32_t kDisplayChoices = 4;

/* Byte offsets inside each layout (components.hpp, packed): every blob starts
   with the format byte, the choice byte follows it, and the label sits after
   the two reserved bytes. */
constexpr uint32_t kChoiceOffset = 1;
constexpr uint32_t kOpsLabelOffset = 4;
constexpr uint32_t kConstantBitsOffset = 4;
constexpr uint32_t kConstantLabelOffset = 8;
constexpr uint32_t kWideBitsOffset = 4;    /* ConstantConfigWide's uint64_t bits */
constexpr uint32_t kWideLabelOffset = 12;

/* One row per type, built from the layout the type registered.  The arrays are
   static because the loader copies the table during the call and only borrows
   the strings for its duration. */
struct Tables {
    std::vector<Field> constant;
    std::vector<Field> display;
    std::vector<Field> ops;
    std::vector<Field> opsPlain;
};

Field textField(const char* id, const char* label, const char* tooltip, uint32_t offset) {
    Field field{};
    field.field_id = id;
    field.label = label;
    field.tooltip = tooltip;
    field.kind = TC_COMPONENT_FIELD_TEXT;
    field.flags = 0;
    field.offset = offset;
    field.bytes = kLabelBytes;   /* includes the terminator, like the blob's own array */
    return field;
}

Field enumField(const char* id, const char* label, const char* tooltip, uint32_t choices,
                const char* const* labels, uint32_t flags) {
    Field field{};
    field.field_id = id;
    field.label = label;
    field.tooltip = tooltip;
    field.kind = TC_COMPONENT_FIELD_ENUM;
    field.flags = flags;
    field.offset = kChoiceOffset;
    field.bytes = 1;
    field.choices = choices;
    field.choice_labels = labels;
    return field;
}

/* The two new widths get the same two rows the FP32 constant has, through the
   loader's width-aware decimal field (TC_COMPONENT_FIELD_FLOAT_WIDTH): 值 is a
   decimal box that shows the shortest text parsing back to the stored pattern
   and accepts inf/nan/-0, and 原始位 is the hexadecimal escape hatch for a NaN
   payload or an exact pattern.  The loader grew that field kind for this Mod:
   its older float box was four bytes of binary32, which is neither binary16's
   two bytes nor binary64's eight. */
std::vector<Field> wideConstantFields(uint32_t format) {
    const uint32_t pattern_bytes = format == 16 ? 2u : 8u;
    std::vector<Field> fields;
    Field value{};
    value.field_id = "value";
    value.label = "值";
    value.tooltip = format == 16 ? "十进制浮点值，写入元件的 FP16 位模式（0x… 见下一行）"
                                 : "十进制浮点值，写入元件的 FP64 位模式（0x… 见下一行）";
    value.kind = TC_COMPONENT_FIELD_FLOAT_WIDTH;
    value.offset = kWideBitsOffset;
    value.bytes = pattern_bytes;
    fields.push_back(value);
    Field bits{};
    bits.field_id = "bits";
    bits.label = "原始位";
    bits.tooltip = format == 16 ? "0x… 的 16 位模式（0x3C00 = 1.0）"
                                : "0x… 的 64 位模式（0x3FF0000000000000 = 1.0）";
    bits.kind = TC_COMPONENT_FIELD_UINT;
    bits.flags = TC_COMPONENT_FIELD_HEX;
    bits.offset = kWideBitsOffset;
    bits.bytes = pattern_bytes;
    bits.bits = format;
    fields.push_back(bits);
    fields.push_back(textField("label", "标签", "显示在本体上的名字（空 = 用默认）",
                               kWideLabelOffset));
    return fields;
}

Tables buildTables(bool editorV2) {
    Tables tables;
    const uint32_t radio = editorV2 ? TC_COMPONENT_FIELD_RADIO : 0u;

    /* 值 + 原始位 + 标签.  The float field writes the binary32 the type stores in
       `bits`; the hex field edits the very same four bytes, which is how a
       special pattern (NaN payload, infinity, denormal) stays reachable now
       that the Mod's own decimal parser is no longer in the loop. */
    Field value{};
    value.field_id = "value";
    value.label = "值";
    value.tooltip = "十进制浮点值，写入元件的 FP32 位模式";
    value.kind = TC_COMPONENT_FIELD_FLOAT;
    value.offset = kConstantBitsOffset;
    value.bytes = 4;
    tables.constant.push_back(value);
    if (editorV2) {
        Field bits{};
        bits.field_id = "bits";
        bits.label = "原始位";
        bits.tooltip = "0x… 的 32 位模式；NaN/Inf/次正规等特殊值从这里写";
        bits.kind = TC_COMPONENT_FIELD_UINT;
        bits.flags = TC_COMPONENT_FIELD_HEX;
        bits.offset = kConstantBitsOffset;
        bits.bytes = 4;
        bits.bits = 32;
        tables.constant.push_back(bits);
    }
    tables.constant.push_back(textField("label", "标签", "显示在本体上的名字（空 = 用默认）",
                                        kConstantLabelOffset));

    tables.display.push_back(enumField("mode", "显示方式", "本元件自己的显示方式",
                                       kDisplayChoices, kDisplayLabels, radio));
    tables.display.push_back(textField("label", "标签", "显示在本体上的名字（空 = 用默认）",
                                       kOpsLabelOffset));

    tables.ops.push_back(enumField("rounding", "舍入模式", "本元件自己的舍入模式",
                                   kRoundingChoices, kRoundingLabels, radio));
    tables.ops.push_back(textField("label", "标签", "显示在本体上的名字（空 = 用默认）",
                                   kOpsLabelOffset));
    tables.opsPlain.push_back(textField("label", "标签", "显示在本体上的名字（空 = 用默认）",
                                        kOpsLabelOffset));
    return tables;
}

}  // namespace

void registerEditorFields(const TCHost* h) {
    if (!h) return;
    ApiV2 editorV2{};
    Api editor{};
    const bool hasV2 = tc::component_editor::tableV2(h, &editorV2);
    if (hasV2) {
        /* The V1 half of the same table, field by field (the two structs share a
           prefix but are different C++ types). */
        editor.size = sizeof(editor);
        editor.version = TC_COMPONENT_EDITOR_API_VERSION_1;
        editor.context = editorV2.context;
        editor.set_fields = editorV2.set_fields;
        editor.clear_fields = editorV2.clear_fields;
    } else if (!tc::component_editor::table(h, &editor)) {
        note("float-ops: this loader has no tc.component.editor; the properties are "
             "stored but there are no rows for them");
        return;
    }

    const Tables tables = buildTables(hasV2);
    int declared = 0;
    const auto declare = [&](uint64_t customId, const std::vector<Field>& fields,
                             const char* what) {
        if (fields.empty()) return;
        const int status = hasV2
                               ? tc::component_editor::setFields(
                                     editorV2, customId, fields.data(),
                                     static_cast<uint32_t>(fields.size()))
                               : tc::component_editor::setFields(
                                     editor, customId, fields.data(),
                                     static_cast<uint32_t>(fields.size()));
        if (status != TC_COMPONENT_EDITOR_OK) {
            char line[208] = {};
            std::snprintf(line, sizeof(line),
                          "float-ops: the loader refused the %s field table: %s (%d)", what,
                          tc::component_editor::errorText(status), status);
            note(line);
            return;
        }
        ++declared;
    };

    /* One table per type, chosen by the layout that type registered: the
       archived binary32 Constant keeps its three rows, the two newer widths get
       the hex row their own width fits, and every operation gets the rounding
       choice only when its arithmetic actually rounds. */
    for (size_t index = 0; index < catalogueCount(); ++index) {
        const CatalogueInfo* info = catalogueAt(index);
        if (!info) continue;
        switch (info->layout) {
            case ConfigLayout::constant_narrow:
                declare(info->id, tables.constant, info->name);
                break;
            case ConfigLayout::constant_wide: {
                const std::vector<Field> fields = wideConstantFields(info->format);
                declare(info->id, fields, info->name);
                break;
            }
            case ConfigLayout::display:
                declare(info->id, tables.display, info->name);
                break;
            case ConfigLayout::ops: default:
                declare(info->id, info->rounding ? tables.ops : tables.opsPlain,
                        info->name);
                break;
        }
    }
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: %d type(s) declared their property rows to the loader "
                  "(tc.component.editor %s)", declared, hasV2 ? "V2" : "V1");
    note(detail);
}

}  // namespace internals
}  // namespace floatops
