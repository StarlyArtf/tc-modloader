/* Editor Demo: a component whose whole state is its configuration, and whose
   editor is a field table rather than a window.

   The type has no pins at all (0 in, 0 out) - it is a decorative object that
   owns a place on the board and 32 bytes of settings.  What makes it an example
   is the call to tc.component.editor: from then on the host draws one row per
   declared field over the game's own component drawer, takes the mouse and
   keyboard itself, and commits each edit through the same configuration path
   tc.component.storage writes use.  The Mod therefore contains no drawing code
   and no input code, only the table below plus one line of logging so a test can
   see the value that landed.

   The five fields cover the V1 kinds: bool, unsigned integer with a range,
   enum, text and float.  Offsets are byte offsets into the blob registered with
   the type; the bytes are a layout, not a C struct, so the packing rules of the
   compiler cannot come between the two. */

#include "../../sdk/tc_component_editor.h"
#include "../../sdk/tc_component_storage.h"
#include "../../sdk/tc_component_types.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const TCHost* host = nullptr;
/* tc.component.storage, used by the custom row's own actions: a menu entry that
   flips a field and a resize grip that writes the dragged size.  Both are
   ordinary configuration writes, so the host's edit transaction around the
   grip turns the whole drag into one undo step. */
tc::component_storage::Api storage{};

constexpr uint64_t kTypeId = UINT64_C(0x454449545f303031);      /* EDIT_001 */
constexpr uint64_t kLongTypeId = UINT64_C(0x454449545f303032);  /* EDIT_002 */
constexpr uint64_t kStagedTypeId = UINT64_C(0x454449545f303033); /* EDIT_003 */
constexpr uint32_t kConfigSchema = 1;
constexpr uint32_t kConfigBytes = 54;

constexpr uint32_t kOffEnabled = 0;   /* bool   1 byte   */
constexpr uint32_t kOffPeriod = 1;    /* uint16, 1..100  */
constexpr uint32_t kOffMode = 3;      /* enum   1 byte   */
constexpr uint32_t kOffGain = 4;      /* f32             */
constexpr uint32_t kOffMask = 8;      /* uint32, hex     */
constexpr uint32_t kOffLabel = 12;    /* text, 24 bytes  */
constexpr uint32_t kOffScale = 36;    /* f32             */
constexpr uint32_t kOffColor = 40;    /* 0xAABBGGRR      */
constexpr uint32_t kOffGrip = 44;     /* int16 dx, int16 dy, written by the custom row */
constexpr uint32_t kOffFormat = 48;   /* enum, drawn as a radio row */
constexpr uint32_t kOffPalette = 50;  /* colour, drawn as a swatch strip + hex box */
constexpr uint32_t kLabelBytes = 24;

const char* const kModes[] = {"RNE", "RTZ", "RDN", "RUP", "RNA"};
const char* const kFormats[] = {"Bin", "Dec", "Hex", "Float"};
/* V4's swatch strip: the same members an enum uses for its choices carry the
   colour literals instead, which is what keeps the frozen field layout. */
const char* const kSwatches[] = {"#ff4040", "#ffa030", "#ffe040", "#40c040", "#4080ff", "#a040ff"};

/* The table shows every V2 shape: a checkbox, a decimal integer, a radio group,
   a pair of fields sharing a line between two separators, a full-row text box
   and an editable hexadecimal word; V3 adds the dropdown enum, the colour, and
   the custom row appended after this table. */
const TCComponentFieldV1 kFields[] = {
    /*  id        label              tooltip  kind                     flags                                          offset       bytes bits choices labels  min max   minf maxf */
    {"enabled", "Enabled",           nullptr, TC_COMPONENT_FIELD_BOOL, 0,                                             kOffEnabled, 1, 0, 0, nullptr, 0, 0, 0.0, 0.0},
    /* V4's advisory marker: these two rows have a validator, so their message
       can be a one-line marker at the end of the row instead of a line of text
       under it.  The message itself still goes to the log. */
    {"period",  "Period (1..100)",   nullptr, TC_COMPONENT_FIELD_UINT, TC_COMPONENT_FIELD_VALIDATION_ICON,             kOffPeriod,  2, 0, 0, nullptr, 1, 100, 0.0, 0.0},
    {"mode",    "Rounding",          nullptr, TC_COMPONENT_FIELD_ENUM, TC_COMPONENT_FIELD_DROPDOWN|TC_COMPONENT_FIELD_VALIDATION_ICON, kOffMode, 1, 0, 5, kModes, 0, 0, 0.0, 0.0},
    {"gain",    "Gain",              nullptr, TC_COMPONENT_FIELD_FLOAT, TC_COMPONENT_FIELD_SEPARATOR,                   kOffGain,    4, 0, 0, nullptr, 0, 0, 0.0, 16.0},
    {"mask",    "Mask (hex)",        nullptr, TC_COMPONENT_FIELD_UINT, TC_COMPONENT_FIELD_HEX | TC_COMPONENT_FIELD_INLINE, kOffMask, 4, 32, 0, nullptr, 0, 0, 0.0, 0.0},
    {"label",   "Label",             nullptr, TC_COMPONENT_FIELD_TEXT, TC_COMPONENT_FIELD_FULL_ROW,                    kOffLabel,   kLabelBytes, 0, 0, nullptr, 0, 0, 0.0, 0.0},
    {"scale",   "Scale (0.25..8)",   nullptr, TC_COMPONENT_FIELD_FLOAT, TC_COMPONENT_FIELD_SEPARATOR,                   kOffScale,   4, 0, 0, nullptr, 0, 0, 0.25, 8.0},
    {"format",  "Number format",     nullptr, TC_COMPONENT_FIELD_ENUM, TC_COMPONENT_FIELD_RADIO,                        kOffFormat,  1, 0, 4, kFormats, 0, 0, 0.0, 0.0},
    {"color",   "Accent",            nullptr, TC_COMPONENT_FIELD_COLOR_RGBA8, 0,                                       kOffColor,   4, 0, 0, nullptr, 0, 0, 0.0, 0.0},
    /* The V4 mixed colour row: a swatch strip for the presets the type cares
       about, and an editable box for the exact 0xAABBGGRR word. */
    {"palette", "Accent preset",     nullptr, TC_COMPONENT_FIELD_COLOR_RGBA8, TC_COMPONENT_FIELD_COLOR_PALETTE|TC_COMPONENT_FIELD_COLOR_HEX, kOffPalette, 4, 0, 6, kSwatches, 0, 0, 0.0, 0.0},
};

/* The scrolling case: thirty-two rows (the field-count ceiling) are taller than
   the screen, so the host caps the row window and lets the player scroll. */
constexpr uint32_t kLongFields = 32;
constexpr uint32_t kLongBytes = kLongFields;
const char* const kLongNames[kLongFields] = {
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9",
    "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19",
    "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29",
    "f30", "f31"};
TCComponentFieldV1 kLongTable[kLongFields] = {};

/* enabled=1, period=16, mode=RNE, gain=2.0, mask=0x00000010, label="hello",
   scale=1.0 - one entry per declared byte, so the table above and this array
   cannot drift apart. */
const uint8_t kDefaultConfig[kConfigBytes] = {
    1,                                     /* enabled           */
    16, 0,                                 /* period = 16       */
    0,                                     /* mode = RNE        */
    0x00, 0x00, 0x00, 0x40,                /* gain = 2.0f       */
    0x10, 0x00, 0x00, 0x00,                /* mask = 0x00000010 */
    'h', 'e', 'l', 'l', 'o',               /* label = "hello"   */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x00, 0x80, 0x3f,                /* scale = 1.0f      */
    0xff, 0xa0, 0x40, 0xff,                /* accent RGBA8      */
    0, 0, 0, 0,                            /* grip dx, dy       */
    0, 0,                                  /* format = Bin, pad */
    0x00, 0x00, 0xc0, 0xff,                /* palette = #0000c0 */
};
static_assert(sizeof(kDefaultConfig) == kConfigBytes, "the default configuration has to fill its own size");

const uint8_t kLongConfig[kLongBytes] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

/* The third type: a table declared with TC_COMPONENT_EDITOR_TABLE_CONFIRM, so
   its rows stage their edits and one Apply writes them all.  Two value fields
   and no custom row - the two shapes are refused together, because a custom row
   writes the configuration itself and the host's staged copy would race it. */
constexpr uint32_t kStagedBytes = 8;
constexpr uint32_t kOffStagedActive = 0;
constexpr uint32_t kOffStagedCount = 1;
constexpr uint32_t kOffStagedChannel = 3;
const char* const kChannels[] = {"Alpha", "Beta", "Gamma", "Delta"};
const uint8_t kStagedConfig[kStagedBytes] = {1, 7, 0, 1, 0, 0, 0, 0};
const TCComponentFieldV1 kStagedFields[] = {
    {"active",  "Active",  nullptr, TC_COMPONENT_FIELD_BOOL, 0,                          kOffStagedActive,  1, 0, 0, nullptr,   0, 0, 0.0, 0.0},
    {"count",   "Count",   nullptr, TC_COMPONENT_FIELD_UINT, 0,                          kOffStagedCount,   2, 0, 0, nullptr,   1, 100, 0.0, 0.0},
    {"channel", "Channel", nullptr, TC_COMPONENT_FIELD_ENUM, TC_COMPONENT_FIELD_DROPDOWN, kOffStagedChannel, 1, 0, 4, kChannels, 0, 0, 0.0, 0.0},
};

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::string hex64(uint64_t value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

uint16_t readU16(const uint8_t* at) {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

float readF32(const uint8_t* at) {
    const uint32_t bits = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
                          (static_cast<uint32_t>(at[2]) << 16) |
                          (static_cast<uint32_t>(at[3]) << 24);
    float value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/* One line per configuration change, decoded from the bytes the host handed
   back.  This is what a playtest asserts on, and what a player can read in the
   log while trying the rows out. */
void reportConfig(uint64_t instance, const uint8_t* config, uint32_t bytes) {
    if (!config || bytes < kConfigBytes) {
        log("editor-demo: instance " + hex64(instance) + " carries " +
            (config ? std::to_string(bytes) : std::string("no")) + " configuration bytes");
        return;
    }
    char label[kLabelBytes + 1]{};
    std::memcpy(label, config + kOffLabel, kLabelBytes);
    const unsigned mode = config[kOffMode];
    uint32_t mask = 0;
    std::memcpy(&mask, config + kOffMask, sizeof(mask));
    uint32_t color=0;std::memcpy(&color,config+kOffColor,sizeof(color));
    uint32_t palette=0;std::memcpy(&palette,config+kOffPalette,sizeof(palette));
    int16_t grip_x=0,grip_y=0;
    std::memcpy(&grip_x,config+kOffGrip,sizeof(grip_x));
    std::memcpy(&grip_y,config+kOffGrip+2,sizeof(grip_y));
    char line[400]{};
    std::snprintf(line, sizeof(line),
                  "editor-demo: instance %s config changed enabled=%u period=%u mode=%s "
                  "gain=%.6g mask=0x%08x label=\"%s\" scale=%.6g color=0x%08x grip=%d,%d "
                  "format=%s palette=0x%08x",
                  hex64(instance).c_str(), unsigned(config[kOffEnabled] ? 1u : 0u),
                  unsigned(readU16(config + kOffPeriod)),
                  mode < 5 ? kModes[mode] : "?", static_cast<double>(readF32(config + kOffGain)),
                  mask, label,
                  static_cast<double>(readF32(config + kOffScale)),color,
                  static_cast<int>(grip_x),static_cast<int>(grip_y),
                  config[kOffFormat] < 4 ? kFormats[config[kOffFormat]] : "?",palette);
    log(line);
}

/* The long type's own change line: twenty booleans, reported as the packed byte
   they live in, so a playtest can see that a click on any of them committed. */
void reportLongConfig(uint64_t instance, const uint8_t* config, uint32_t bytes) {
    if (!config || bytes < kLongBytes) {
        log("editor-demo: instance " + hex64(instance) + " carries " +
            (config ? std::to_string(bytes) : std::string("no")) + " long configuration bytes");
        return;
    }
    unsigned ones = 0;
    for (uint32_t index = 0; index < kLongBytes; ++index)
        if (config[index]) ++ones;
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "editor-demo: long instance %s config changed f0=%u f31=%u ones=%u",
                  hex64(instance).c_str(), unsigned(config[0] ? 1u : 0u),
                  unsigned(config[kLongBytes - 1] ? 1u : 0u), ones);
    log(line);
}

/* The staged type's own change line, written once per Apply (or not at all,
   when the player cancelled): both fields travel in the one line, which is what
   makes "one write for the whole batch" visible from outside. */
void reportStagedConfig(uint64_t instance, const uint8_t* config, uint32_t bytes) {
    if (!config || bytes < kStagedBytes) {
        log("editor-demo: staged instance " + hex64(instance) + " carries " +
            (config ? std::to_string(bytes) : std::string("no")) + " configuration bytes");
        return;
    }
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "editor-demo: staged instance %s config changed active=%u count=%u channel=%s",
                  hex64(instance).c_str(), unsigned(config[kOffStagedActive] ? 1u : 0u),
                  unsigned(readU16(config + kOffStagedCount)),
                  config[kOffStagedChannel] < 4 ? kChannels[config[kOffStagedChannel]] : "?");
    log(line);
}

/* The V2 entry point requires a simulation callback even for a type with no
   pins; this component's behaviour is entirely its configuration. */
void logic(TCLogicIOV2* io) { (void)io; }

void onConfigChanged(TCLogicIOV2* io) {
    if (!io) return;
    /* The three types share one callback; each report is written for the size
       its own configuration has. */
    if (io->config_size == kStagedBytes) {
        reportStagedConfig(io->instance_id, io->config, io->config_size);
    } else if (io->config_size == kLongBytes) {
        reportLongConfig(io->instance_id, io->config, io->config_size);
    } else {
        reportConfig(io->instance_id, io->config, io->config_size);
    }
}

/* Only the one callback: a lifecycle block whose `size` stops right after it
   keeps the load/save and clone notifications off, exactly as documented. */
const TCComponentLifecycleV1 kLifecycle = [] {
    TCComponentLifecycleV1 value{};
    value.size = offsetof(TCComponentLifecycleV1, on_config_changed) +
                 sizeof(value.on_config_changed);
    value.version = TC_COMPONENT_LIFECYCLE_VERSION_1;
    value.on_config_changed = &onConfigChanged;
    return value;
}();

int validatePeriod(void*,const TCComponentInstanceHandle*,const char*,const uint8_t* config,
                   uint32_t bytes,char* message,uint32_t capacity) {
    if(!config||bytes<kConfigBytes)return TC_COMPONENT_EDITOR_VALIDATION_ERROR;
    const uint16_t period=readU16(config+kOffPeriod);
    if(period==13){
        if(message&&capacity)std::snprintf(message,capacity,"13 is reserved by this demo");
        return TC_COMPONENT_EDITOR_VALIDATION_ERROR;
    }
    if(period&1u){
        if(message&&capacity)std::snprintf(message,capacity,"odd periods jitter in the demo");
        return TC_COMPONENT_EDITOR_VALIDATION_WARNING;
    }
    return TC_COMPONENT_EDITOR_VALIDATION_OK;
}

/* The enum validator is the one the playtest drives: a dropdown click commits in
   one gesture, so "pick the refused choice" is a deterministic way to reach both
   validator outcomes through real input - the last choice is refused (the
   configuration keeps its old bytes) and one earlier choice only warns. */
int validateMode(void*,const TCComponentInstanceHandle*,const char*,const uint8_t* config,
                 uint32_t bytes,char* message,uint32_t capacity) {
    if(!config||bytes<kConfigBytes)return TC_COMPONENT_EDITOR_VALIDATION_ERROR;
    if(config[kOffMode]==4){
        if(message&&capacity)std::snprintf(message,capacity,"the demo refuses RNA");
        return TC_COMPONENT_EDITOR_VALIDATION_ERROR;
    }
    if(config[kOffMode]==1){
        if(message&&capacity)std::snprintf(message,capacity,"RTZ is lossy in this demo");
        return TC_COMPONENT_EDITOR_VALIDATION_WARNING;
    }
    return TC_COMPONENT_EDITOR_VALIDATION_OK;
}

void drawPreview(void*,const TCComponentEditorCustomRowV1* row) {
    if(!row||!row->draw||!row->config||row->config_size<kConfigBytes)return;
    uint32_t color=0;std::memcpy(&color,row->config+kOffColor,sizeof(color));
    row->draw->rect_filled(row->draw->context,row->min_x,row->min_y,row->max_x,row->max_y,
                           color,5.f);
    row->draw->text(row->draw->context,row->min_x+8.f,row->min_y+6.f,0xffffffffu,
                    "custom row: right-click / double-click / drag grip");
}

void previewAction(void*,const TCComponentEditorActionV1* action) {
    if(!action)return;
    log("editor-demo: custom action kind="+std::to_string(action->kind)+
        " id="+std::to_string(action->action_id)+" delta="+
        std::to_string(action->delta_x)+","+std::to_string(action->delta_y));
    if(!storage.write_config||!action->config||action->config_size<kConfigBytes)return;
    /* The menu entry edits a declared field; the grip writes the accumulated drag
       into the configuration.  Both are ordinary writes, so the host's resize
       transaction (opened when the grip was pressed) makes the whole drag one
       undo step, and both end up in the instance's on_config_changed line. */
    std::vector<uint8_t> next(action->config,action->config+kConfigBytes);
    bool write=false;
    if(action->kind==TC_COMPONENT_EDITOR_ACTION_MENU&&action->action_id==1){
        next[kOffEnabled]=next[kOffEnabled]?0:1;
        write=true;
    }
    if(action->kind==TC_COMPONENT_EDITOR_ACTION_RESIZE||
       action->kind==TC_COMPONENT_EDITOR_ACTION_RESIZE_END){
        const auto clamp=[](float value){
            const long rounded=std::lround(value);
            return static_cast<int16_t>(rounded<-32768?-32768:(rounded>32767?32767:rounded));
        };
        const int16_t dx=clamp(action->delta_x),dy=clamp(action->delta_y);
        std::memcpy(next.data()+kOffGrip,&dx,sizeof(dx));
        std::memcpy(next.data()+kOffGrip+2,&dy,sizeof(dy));
        write=true;
    }
    if(!write)return;
    const int status=tc::component_storage::writeConfig(storage,action->instance,
                                                       kConfigSchema,next.data(),kConfigBytes);
    if(status!=TC_COMPONENT_STORAGE_OK)
        log("editor-demo: the custom action's configuration write was refused ("+
            std::to_string(status)+")");
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out) return 1;
    if (h->api_version != TC_MOD_API_VERSION) return 2;
    if (!tc::hostHas(h, TC_CAP_SERVICES)) return 3;
    host = h;
    /* The custom row's actions write configuration through the storage service;
       without it the rows still draw and edit, only those two actions are
       disabled. */
    if (!tc::component_storage::table(h, &storage))
        log("editor-demo: this loader has no tc.component.storage");

    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) {
        log("editor-demo: this loader has no tc.component.types");
        return 4;
    }
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kTypeId;
    definition.type_id = "local.editor-demo/settings";
    definition.name = "Editor Demo";
    definition.description =
        "A component whose state is its configuration: the host draws the "
        "property rows from the type's field table (tc.component.editor).";
    definition.input_count = 0;
    definition.output_count = 0;
    definition.state_words = 0;
    definition.gate_cost = 0;
    definition.delay = 0;
    definition.callback = &logic;
    definition.lifecycle = &kLifecycle;
    definition.config_schema = kConfigSchema;
    definition.config_size = kConfigBytes;
    definition.default_config = kDefaultConfig;
    const int status = tc::component_types::registerDefinition(types, &definition);
    if (status != TC_COMPONENT_TYPES_OK) {
        log(std::string("editor-demo: the type was refused: ") +
            tc::component_types::errorText(status));
        return 5;
    }
    /* The same shape of component with more rows than fit under the drawer: the
       host caps its row window and scrolls, which is what the playtest's long
       case checks. */
    for (uint32_t index = 0; index < kLongFields; ++index) {
        kLongTable[index].field_id = kLongNames[index];
        kLongTable[index].label = kLongNames[index];
        kLongTable[index].kind = TC_COMPONENT_FIELD_BOOL;
        kLongTable[index].offset = index;
        kLongTable[index].bytes = 1;
    }
    TCComponentTypeDefinitionV2 longDefinition = definition;
    longDefinition.custom_id = kLongTypeId;
    longDefinition.type_id = "local.editor-demo/long";
    longDefinition.name = "Editor Demo (long)";
    longDefinition.description = "Twenty rows: the host's editor window scrolls.";
    longDefinition.config_schema = kConfigSchema + 1;
    longDefinition.config_size = kLongBytes;
    longDefinition.default_config = kLongConfig;
    /* The opt-in half of the description rule: this type asks the loader to put
       its description into the game's prototype, so the bottom panel prints it.
       The settings type above deliberately does not, which is the new default. */
    longDefinition.flags = TC_COMPONENT_TYPE_SHOW_DESCRIPTION;
    const int longStatus = tc::component_types::registerDefinition(types, &longDefinition);
    if (longStatus != TC_COMPONENT_TYPES_OK) {
        log(std::string("editor-demo: the long type was refused: ") +
            tc::component_types::errorText(longStatus));
        return 5;
    }

    /* The staged type: its rows are ordinary value rows, but the table is
       declared with CONFIRM, so an edit lands in the host's working copy and one
       Apply writes the batch. */
    TCComponentTypeDefinitionV2 stagedDefinition = definition;
    stagedDefinition.custom_id = kStagedTypeId;
    stagedDefinition.type_id = "local.editor-demo/staged";
    stagedDefinition.name = "Editor Demo (staged)";
    stagedDefinition.description = "Two rows that stage their edits behind Apply / Cancel.";
    stagedDefinition.config_schema = kConfigSchema + 2;
    stagedDefinition.config_size = kStagedBytes;
    stagedDefinition.default_config = kStagedConfig;
    const int stagedStatus = tc::component_types::registerDefinition(types, &stagedDefinition);
    if (stagedStatus != TC_COMPONENT_TYPES_OK) {
        log(std::string("editor-demo: the staged type was refused: ") +
            tc::component_types::errorText(stagedStatus));
        return 5;
    }

    /* The editor is the whole point of this Mod, so its absence is a failed
       load rather than a component with no properties. */
    tc::component_editor::ApiV4 editor{};
    if (!tc::component_editor::tableV4(h, &editor)) {
        log("editor-demo: this loader has no tc.component.editor");
        return 6;
    }
    const uint32_t count = static_cast<uint32_t>(sizeof(kFields) / sizeof(kFields[0]));
    std::vector<TCComponentFieldV2> fields(count+1);
    for(uint32_t index=0;index<count;++index){
        fields[index].size=sizeof(TCComponentFieldV2);
        fields[index].version=TC_COMPONENT_FIELD_DEFINITION_VERSION_2;
        fields[index].field=kFields[index];
    }
    fields[1].validate=&validatePeriod;
    fields[2].validate=&validateMode;
    static const uint32_t actionIds[]={1,2};
    static const char* const actionLabels[]={"First action","Second action"};
    TCComponentFieldV2& preview=fields.back();
    preview.size=sizeof(preview);preview.version=TC_COMPONENT_FIELD_DEFINITION_VERSION_2;
    preview.field.field_id="preview";preview.field.label="Preview";
    preview.field.kind=TC_COMPONENT_FIELD_CUSTOM;
    preview.field.flags=TC_COMPONENT_FIELD_FULL_ROW|TC_COMPONENT_FIELD_RESIZE_HANDLE|
                        TC_COMPONENT_FIELD_SEPARATOR;
    preview.draw_custom=&drawPreview;preview.action=&previewAction;preview.custom_height=42.f;
    preview.context_actions=2;preview.context_action_ids=actionIds;
    preview.context_action_labels=actionLabels;
    const int editorStatus = tc::component_editor::setFieldsV2(
        editor,kTypeId,fields.data(),static_cast<uint32_t>(fields.size()));
    if (editorStatus != TC_COMPONENT_EDITOR_OK) {
        log(std::string("editor-demo: the field table was refused: ") +
            tc::component_editor::errorText(editorStatus));
        return 7;
    }
    log("editor-demo: declared " + std::to_string(fields.size()) + " editor fields for type " +
        hex64(kTypeId));
    const int longEditorStatus =
        tc::component_editor::setFields(editor, kLongTypeId, kLongTable, kLongFields);
    if (longEditorStatus != TC_COMPONENT_EDITOR_OK) {
        log(std::string("editor-demo: the long field table was refused: ") +
            tc::component_editor::errorText(longEditorStatus));
        return 7;
    }
    log("editor-demo: declared " + std::to_string(kLongFields) + " editor fields for type " +
        hex64(kLongTypeId));
    /* The staged table goes through the V4 entry point so it can carry its
       table-level flag; the two tables above stay on the V3 call, which is what
       keeps their meaning frozen. */
    const uint32_t stagedFieldCount =
        static_cast<uint32_t>(sizeof(kStagedFields) / sizeof(kStagedFields[0]));
    std::vector<TCComponentFieldV2> stagedFields(stagedFieldCount);
    for (uint32_t index = 0; index < stagedFieldCount; ++index) {
        stagedFields[index].size = sizeof(TCComponentFieldV2);
        stagedFields[index].version = TC_COMPONENT_FIELD_DEFINITION_VERSION_2;
        stagedFields[index].field = kStagedFields[index];
    }
    const int stagedEditorStatus = tc::component_editor::setFieldsV3(
        editor, kStagedTypeId, stagedFields.data(), stagedFieldCount,
        TC_COMPONENT_EDITOR_TABLE_CONFIRM);
    if (stagedEditorStatus != TC_COMPONENT_EDITOR_OK) {
        log(std::string("editor-demo: the staged field table was refused: ") +
            tc::component_editor::errorText(stagedEditorStatus));
        return 7;
    }
    log("editor-demo: declared " + std::to_string(stagedFieldCount) +
        " staged editor fields for type " + hex64(kStagedTypeId));
    return 0;
}
