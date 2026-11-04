/* The float catalogue: Constant, Add and Display, then every operation of the
   family, each one registered once per IEEE 754 format.

   One format is one component type (plan 6.4.1): a Mod type's pin widths are
   frozen when it registers, so "the same Add, switched between 16/32/64" is not
   expressible - FP16 Add, FP32 Add and FP64 Add are three types with three pin
   sets, and the width box on the body only reports which one an instance is.
   The configuration layouts are the archived contract (plan 7.1): every blob
   starts with a `format` byte, so a record always says which width it was
   written for and one written by another width is refused instead of being read
   at the wrong size. */

#ifndef float_ops_components_hpp
#define float_ops_components_hpp

#include "../sdk/tc_component_types.h"

#include <cstddef>
#include <cstdint>

namespace floatops {

/* Stable identities: they are what a saved circuit stores, so they never
   change.  Every family has three of them and the three differ only in the two
   width digits (`F16ADD_1`, `F32ADD_1`, `F64ADD_1`), so they are built from the
   family's five trailing characters.  The named constants below keep the
   hexadecimal spelling the earlier releases shipped, and the static assertions
   prove the builder reproduces them byte for byte. */
constexpr uint64_t floatTypeId(uint32_t format, const char* tail) {
    uint64_t id = static_cast<uint64_t>('F') << 56;
    id |= static_cast<uint64_t>('0' + (format / 10)) << 48;
    id |= static_cast<uint64_t>('0' + (format % 10)) << 40;
    for (uint32_t index = 0; index < 5; ++index)
        id |= static_cast<uint64_t>(static_cast<unsigned char>(tail[index]))
              << (32 - 8 * index);
    return id;
}

constexpr uint64_t kConstantId = UINT64_C(0x463332434f4e5331); /* F32CONS1 */
constexpr uint64_t kAddId = UINT64_C(0x4633324144445f31);      /* F32ADD_1 */
constexpr uint64_t kDisplayId = UINT64_C(0x4633324449535031);  /* F32DISP1 */

/* The M3/M4 catalogue (plan 6.2 and 6.3).  These identities are saved in
   circuits, so they never change; the three above keep their own configuration
   layouts because they shipped first, and every type below shares the one
   "ops" layout (format + rounding + label). */
constexpr uint64_t kSubtractId = UINT64_C(0x4633325355425f31); /* F32SUB_1 */
constexpr uint64_t kMultiplyId = UINT64_C(0x4633324d554c5f31); /* F32MUL_1 */
constexpr uint64_t kDivideId = UINT64_C(0x4633324449565f31);   /* F32DIV_1 */
constexpr uint64_t kSquareRootId = UINT64_C(0x4633325351545f31);/* F32SQT_1 */
constexpr uint64_t kNegateId = UINT64_C(0x4633324e45475f31);   /* F32NEG_1 */
constexpr uint64_t kAbsoluteId = UINT64_C(0x4633324142535f31); /* F32ABS_1 */
constexpr uint64_t kCompareId = UINT64_C(0x463332434d505f31);  /* F32CMP_1 */
constexpr uint64_t kClassifyId = UINT64_C(0x463332434c535f31); /* F32CLS_1 */
constexpr uint64_t kFusedMultiplyAddId = UINT64_C(0x463332464d415f31); /* F32FMA_1 */
constexpr uint64_t kRemainderId = UINT64_C(0x46333252454d5f31);/* F32REM_1 */
constexpr uint64_t kRoundToIntegralId = UINT64_C(0x463332524e445f31);  /* F32RND_1 */
constexpr uint64_t kMinimumId = UINT64_C(0x4633324d494e5f31);  /* F32MIN_1 */
constexpr uint64_t kMaximumId = UINT64_C(0x4633324d41585f31);  /* F32MAX_1 */
constexpr uint64_t kI32ToFp32Id = UINT64_C(0x4633324932465f31);/* F32I2F_1 */
constexpr uint64_t kU32ToFp32Id = UINT64_C(0x4633325532465f31);/* F32U2F_1 */
constexpr uint64_t kFp32ToI32Id = UINT64_C(0x4633324632495f31);/* F32F2I_1 */
constexpr uint64_t kFp32ToU32Id = UINT64_C(0x4633324632555f31);/* F32F2U_1 */
constexpr uint64_t kSplitBitsId = UINT64_C(0x46333253504c5f31);/* F32SPL_1 */
constexpr uint64_t kMakeBitsId = UINT64_C(0x4633324d4b425f31); /* F32MKB_1 */

static_assert(floatTypeId(32, "CONS1") == kConstantId, "F32CONS1");
static_assert(floatTypeId(32, "ADD_1") == kAddId, "F32ADD_1");
static_assert(floatTypeId(32, "DISP1") == kDisplayId, "F32DISP1");
static_assert(floatTypeId(32, "SUB_1") == kSubtractId, "F32SUB_1");
static_assert(floatTypeId(32, "MUL_1") == kMultiplyId, "F32MUL_1");
static_assert(floatTypeId(32, "DIV_1") == kDivideId, "F32DIV_1");
static_assert(floatTypeId(32, "SQT_1") == kSquareRootId, "F32SQT_1");
static_assert(floatTypeId(32, "NEG_1") == kNegateId, "F32NEG_1");
static_assert(floatTypeId(32, "ABS_1") == kAbsoluteId, "F32ABS_1");
static_assert(floatTypeId(32, "CMP_1") == kCompareId, "F32CMP_1");
static_assert(floatTypeId(32, "CLS_1") == kClassifyId, "F32CLS_1");
static_assert(floatTypeId(32, "FMA_1") == kFusedMultiplyAddId, "F32FMA_1");
static_assert(floatTypeId(32, "REM_1") == kRemainderId, "F32REM_1");
static_assert(floatTypeId(32, "RND_1") == kRoundToIntegralId, "F32RND_1");
static_assert(floatTypeId(32, "MIN_1") == kMinimumId, "F32MIN_1");
static_assert(floatTypeId(32, "MAX_1") == kMaximumId, "F32MAX_1");
static_assert(floatTypeId(32, "I2F_1") == kI32ToFp32Id, "F32I2F_1");
static_assert(floatTypeId(32, "U2F_1") == kU32ToFp32Id, "F32U2F_1");
static_assert(floatTypeId(32, "F2I_1") == kFp32ToI32Id, "F32F2I_1");
static_assert(floatTypeId(32, "F2U_1") == kFp32ToU32Id, "F32F2U_1");
static_assert(floatTypeId(32, "SPL_1") == kSplitBitsId, "F32SPL_1");
static_assert(floatTypeId(32, "MKB_1") == kMakeBitsId, "F32MKB_1");

/* 3 adds the per-instance label (the text the player types in the game's own
   component drawer, shown where the stock parts show CONST/STATIC); 2 is the
   layout with the format byte but no label, and 1 was the pre-format layout
   that only a development build ever wrote (see migrateLegacyConfig below).
   Only the three binary32 types above ever shipped 1 or 2; binary16/binary64
   are new types whose records are always schema 3. */
constexpr uint32_t kConfigSchema = 3;
constexpr uint32_t kLabelSchema = 2;
constexpr uint32_t kLegacyConfigSchema = 1;

/* Longest label a component can carry, NUL included.  The stock drawer's own
   field is wider than a component's corner, so what matters is that the string
   fits the body's name slot at the board's largest zoom; 15 characters is the
   same order as the game's own level-IO names and keeps the configuration blob
   small enough for the 1024-byte budget (plan 3). */
constexpr size_t kLabelBytes = 16;

#pragma pack(push, 1)
/* The archived binary32 Constant: four bytes of pattern with the label right
   after them.  binary32 shipped first, so this layout cannot grow without
   moving the label of every constant already in a player's save. */
struct ConstantConfig {
    uint8_t format;
    uint8_t display;
    uint16_t reserved;
    uint32_t bits;
    char label[kLabelBytes];
};
/* What binary16 and binary64 use instead: the same three bytes of header, a
   64-bit pattern field, then the label.  A separate layout rather than a wider
   ConstantConfig, because the FP32 blob above is already archived; binary16
   keeps its two bytes in the low half of the 64-bit field, and its drawer row
   is declared with the format's own width so the upper bytes stay zero. */
struct ConstantConfigWide {
    uint8_t format;
    uint8_t display;
    uint16_t reserved;
    uint64_t bits;
    char label[kLabelBytes];
};
struct AddConfig {
    uint8_t format;
    uint8_t rounding;
    uint16_t reserved;
    char label[kLabelBytes];
};
struct DisplayConfig {
    uint8_t format;
    uint8_t mode;
    uint16_t reserved;
    char label[kLabelBytes];
};

/* The catalogue's own configuration: the format byte every blob starts with,
   the instance's rounding mode (meaningful only for the types whose arithmetic
   rounds) and the label the board prints.  It is byte-for-byte the AddConfig
   layout, so one decode and one migration path serve all of them. */
struct OpsConfig {
    uint8_t format;
    uint8_t rounding;
    uint16_t reserved;
    char label[kLabelBytes];
};
#pragma pack(pop)

/* What the drawer and the renderer have to know about one catalogue type.
   The registration table in components.cpp is the single source of truth: the
   rows in the game's own panel ask for it by custom id instead of repeating the
   same list. */
/* Which configuration blob a type registers.  Every blob starts with the format
   byte, but the three families differ after it (components.hpp's structs), and
   the drawer needs to know which rows a type gets. */
enum class ConfigLayout : uint8_t { ops, constant_narrow, constant_wide, display };

/* What the body prints in the middle (plan 7.2). */
enum class BodyKind : uint8_t { operator_symbol, constant_value, display_text };

struct CatalogueInfo {
    uint64_t id;
    uint32_t format;         /* 16, 32 or 64 - the width this type works in */
    ConfigLayout layout;     /* which configuration layout it registered */
    BodyKind body;           /* what its body prints */
    const char* type_id;     /* "local.float-ops/fp32-multiply" */
    const char* group;       /* "计算" - the folder under 浮点/FP32 this type lives in */
    const char* name;        /* "FP32 Multiply" */
    const char* description; /* the drawer's own description text */
    const char* board_name;  /* what an unlabelled instance prints (MUL) */
    const char* symbol;      /* what the body prints in the middle (×) */
    bool rounding;           /* stores and shows a rounding mode */
};

/* The catalogue, in registration order (never NULL; count is its length). */
size_t catalogueCount();
const CatalogueInfo* catalogueAt(size_t index);
/* The entry for one custom id, or nullptr when the id is not this Mod's. */
const CatalogueInfo* catalogueInfo(uint64_t custom_id);

/* Registration plus the frame callback that drives the editors.  Returns false
   when the loader cannot provide what the components need; the caller then
   reports it. */
bool initialize(const TCHost* host, TCPlugin* plugin);

/* For the true-game test: the number of live instances whose configuration the
   plugin could read, and the display text it last produced for one instance. */
int liveInstanceCount();
size_t displayText(uint64_t instance, char* out, size_t capacity);

}  // namespace floatops

#endif
