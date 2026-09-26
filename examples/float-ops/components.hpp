/* The M2 vertical slice: FP32 Constant, FP32 Add and FP32 Display.

   The three types exist to prove the whole path - configuration, arithmetic,
   text and the game's own rendering - before the rest of the catalogue is
   built.  Their configuration layouts are the archived contract (plan 7.1):
   every blob starts with a `format` byte whose only legal value today is 32,
   so binary16/binary64 can be added later without a new schema. */

#ifndef float_ops_components_hpp
#define float_ops_components_hpp

#include "../sdk/tc_component_types.h"

#include <cstddef>
#include <cstdint>

namespace floatops {

/* Stable identities: they are what a saved circuit stores, so they never
   change. */
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

/* 3 adds the per-instance label (the text the player types in the game's own
   component drawer, shown where the stock parts show CONST/STATIC); 2 is the
   layout with the format byte but no label, and 1 was the pre-format layout
   that only a development build ever wrote (see migrate() below). */
constexpr uint32_t kConfigSchema = 3;
constexpr uint32_t kLabelSchema = 2;
constexpr uint32_t kLegacyConfigSchema = 1;

/* Longest label a component can carry, NUL included.  The stock drawer's own
   field is wider than a component's corner, so what matters is that the string
   fits the body's name slot at the board's largest zoom; 15 characters is the
   same order as the game's own level-IO names and keeps the configuration blob
   small enough for the 1024-byte budget (plan 3). */
constexpr size_t kLabelBytes = 16;

/* The only legal format value in this release (plan 7.1). */
constexpr uint8_t kFormatBinary32 = 32;

#pragma pack(push, 1)
struct ConstantConfig {
    uint8_t format;
    uint8_t display;
    uint16_t reserved;
    uint32_t bits;
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
struct CatalogueInfo {
    uint64_t id;
    const char* type_id;      /* "local.float-ops/fp32-multiply" */
    const char* name;         /* "FP32 Multiply" */
    const char* description;  /* the drawer's own description text */
    const char* board_name;   /* what an unlabelled instance prints (MUL) */
    const char* symbol;       /* what the body prints in the middle (x) */
    bool rounding;            /* stores and shows a rounding mode */
    const char* hint;         /* the drawer's second row when there is no
                                 rounding choice (bit order, saturating policy,
                                 pin meaning) */
};

/* The catalogue, in registration order (never NULL; count is its length). */
size_t catalogueCount();
const CatalogueInfo* catalogueAt(size_t index);
/* The entry for one custom id, or nullptr for the three M2 types. */
const CatalogueInfo* catalogueInfo(uint64_t custom_id);

/* Registration plus the frame callback that drives the editors.  Returns false
   when the loader cannot provide what M2 needs; the caller then reports it. */
bool initialize(const TCHost* host, TCPlugin* plugin);

/* For the true-game test: the number of live instances whose configuration the
   plugin could read, and the display text it last produced for one instance. */
int liveInstanceCount();
size_t displayText(uint64_t instance, char* out, size_t capacity);

}  // namespace floatops

#endif
