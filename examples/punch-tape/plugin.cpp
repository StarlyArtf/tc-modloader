/* Punch tape: set wide values with the game's own kind of bit squares.

   The game's board UI draws a row of clickable bit squares for pins of eight
   bits or less and nothing but a number box above that (measured: the IO panel
   branches on `cmp width,0x8`, and the component drawer only has a text field
   for a constant).  This Mod draws the same idea for any width both in the
   constant drawer and in the component workshop's native left-side input
   settings.  Bits stay arranged in bytes: eight per group and up to four
   groups per row.  Each host panel gets its own scale floor, so even a narrow
   side panel keeps the whole tape visible.

   It persists edits through the game's own setting entry point.  On loaders
   with runtime-wide-constant support, the generated simulator reads the new
   value from a small shared slot and can refresh immediately; older loaders
   retain the native full-recompile path as a compatibility fallback:

     * a constant (the drawer's "value" settings[0]) goes through
       set_setting(board, 0, componentIndex, value);
     * tc_dynamic_constant_set updates the compiled simulator's runtime value;
     * if that export is unavailable, the drawer's presenter context is upgraded
       to state 0x30, exactly like the native value field.

   Both take the component index into the board's component array
   (board + 0x78 count, board + 0x80 data, stride 0x238). */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_ui_texture.h"
#include "../../sdk/tc_handle_api.h"
#include "../../sdk/tc_io_value.h"
#include "../../sdk/tc_hook.h"
#include "../../sdk/tc_pin_order.h"
/* Layout of the tape itself: pure arithmetic, tested by tests/punch-tape-layout.cpp. */
#include "tape_layout.hpp"
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

/* The game's own punch-tape sprite: asset/io_state/io_state.png, 200x1100, with
   a 96x96 pink chip at (2,2) and a green one at (102,2) - the same picture the
   game uses for its bit squares, read from the player's installation at runtime
   (nothing is redistributed) and uploaded through the host's texture API. */
static const wchar_t* kSpriteRelative = L"asset\\io_state\\io_state.png";
static constexpr float kSpriteWidth = 200.f;
static constexpr float kSpriteHeight = 1100.f;
static constexpr float kChipX0[2] = {2.f, 102.f};
/* The first three atlas rows are the game's normal, hovered and pressed
   versions of the same red/green chip. */
static constexpr float kChipY[3] = {2.f, 102.f, 202.f};
static constexpr float kChipSize = 96.f;

static tc::ui::Texture chipTexture;
static bool chipTextureTried;

static bool decodePng(const std::wstring& path, std::vector<unsigned char>& rgba,
                      uint32_t* width, uint32_t* height) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    do {
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()),
                                         rgba.data())))
            break;
        *width = w;
        *height = h;
        ok = true;
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

static void ensureChipTexture() {
    if (chipTextureTried) return;
    chipTextureTried = true;
    wchar_t exePath[32768]{};
    if (!GetModuleFileNameW(nullptr, exePath, 32768)) return;
    std::wstring path(exePath);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.resize(slash + 1);
    path += kSpriteRelative;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::vector<unsigned char> rgba;
    uint32_t width = 0, height = 0;
    if (!decodePng(path, rgba, &width, &height)) return;
    chipTexture.createRgba(width, height, rgba.data(), rgba.size(),
                           tc::ui::TextureFilter::Linear);
}

static const TCHost* host;

using SetSettingFn = void (*)(void* board, int64_t setting, int64_t component, int64_t value);
static SetSettingFn setSetting;
using UpgradeContextFn = void (*)(void* contextState, uint8_t state);
static UpgradeContextFn upgradeContext;
using StopAndRefreshFn = void (*)(void* board);
static StopAndRefreshFn stopAndRefresh;
using GetGlobalInputFn = int64_t (*)(void* board, int64_t component);
static GetGlobalInputFn getGlobalInputOriginal;
using SetGlobalInputFn = void (*)(void* board, int64_t component, int64_t value);
static SetGlobalInputFn setGlobalInputOriginal;
using FlipGlobalInputFn = void (*)(void* board, int64_t component, int64_t bit);
static FlipGlobalInputFn flipGlobalInput;
using SetDynamicConstantFn = void (*)(uint64_t component, uint64_t value);
static SetDynamicConstantFn setDynamicConstant;
/* Measured in this game's component-description builder: immediately after a
   native setting commit it calls upgrade(secondArgument + 0x1a3b8, 0x30). */
static constexpr uint64_t kContextStateOffset = 0x1a3b8;
static constexpr uint8_t kSettingsChangedState = 0x30;

/* The component drawer draws the constant's bits as a read-only picture.  The
   loader hands the mouse to a board panel, not to that window, so the tape is
   drawn from inside the drawer instead: hook its builder, let it lay out its own
   widgets, then append the clickable squares in the same window.

   The builder's arguments are forwarded untouched.  Its parameter list is not a
   published contract, so the detour takes them as opaque words: the first four
   travel in rcx/rdx/r8/r9 and the rest on the stack, which is exactly what the
   game's own call site sets up (measured at build_bottom_panel + 0xd8d). */
using PanelFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
static PanelFn panelOriginal;
static bool panelSeen;
static int layoutReports;
static uint8_t* ioStateView;
/* Return address immediately after build_io_state_view's call to
   get_component_global_input.  Checking it keeps the helper hook inert when
   the same game function is used outside the native IO panel. */
static constexpr uintptr_t kIoInputValueCallReturn = 0x779;

/* The component sequence is a Nim seq: its payload has an eight-byte header,
   then 0x238-byte records.  All offsets below are record-relative.  A real
   64-bit constant dump gives kind 0x2e at +0x00, one setting at +0xa8/+0xb0,
   and its output word size at +0xe0 (mirrored at +0xe8 on this build). */
static constexpr uint64_t kRecordHeader = 8;
static constexpr uint64_t kSettingCountOffset = 0xa8;
static constexpr uint64_t kSettingsOffset = 0xb0;
static constexpr uint64_t kValueWidthOffset = 0xe0;
static constexpr uint64_t kValueWidthMirrorOffset = 0xe8;
static constexpr uint8_t kConstantKind = 0x2e;

static constexpr uint64_t kComponentStride = 0x238;
/* setting[0] is what the component drawer's "value" field writes. */
static constexpr int64_t kValueSetting = 0;

struct BoardView {
    void* board = nullptr;
    uint8_t* data = nullptr;
    uint64_t count = 0;
};

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static BoardView boardView() {
    BoardView view;
    TCGameHandle handle{};
    if (!host || tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) return view;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &handle, &raw) != TC_HANDLE_OK || !raw) return view;
    auto* bytes = const_cast<uint8_t*>(static_cast<const uint8_t*>(raw));
    memcpy(&view.count, bytes + 0x78, sizeof(view.count));
    memcpy(&view.data, bytes + 0x80, sizeof(view.data));
    if (!view.data || view.count == 0 || view.count > 1000000) return BoardView{};
    view.board = bytes;
    return view;
}

static uint8_t* componentAt(const BoardView& view, uint64_t index) {
    return view.data + kRecordHeader + index * kComponentStride;
}

static uint8_t componentKind(const uint8_t* component) {
    return component[0x00];
}

static int componentWidth(const uint8_t* component, int fallback = 8) {
    uint64_t width = 0, mirror = 0;
    memcpy(&width, component + kValueWidthOffset, sizeof(width));
    memcpy(&mirror, component + kValueWidthMirrorOffset, sizeof(mirror));
    if (width >= 1 && width <= 64) return static_cast<int>(width);
    if (mirror >= 1 && mirror <= 64) return static_cast<int>(mirror);
    return fallback >= 1 && fallback <= 64 ? fallback : 8;
}

/* The component's settings array (int64 each).  A constant's value is entry 0;
   the same array is what the drawer's fields write. */
static int64_t componentSettingCount(const uint8_t* component) {
    int64_t count = 0;
    memcpy(&count, component + kSettingCountOffset, sizeof(count));
    return (count > 0 && count < 1024) ? count : 0;
}

static bool componentSetting(const uint8_t* component, int64_t index, int64_t* out) {
    if (index < 0 || index >= componentSettingCount(component)) return false;
    const uint8_t* array = nullptr;
    memcpy(&array, component + kSettingsOffset, sizeof(array));
    if (!array) return false;
    /* The game's own set_setting writes [array + 8 + index*8]: the array is a Nim
       seq, so the payload starts eight bytes in. Reading index*8 alone returns
       the sequence header instead of the setting. */
    memcpy(out, array + 8 + index * 8, sizeof(*out));
    return true;
}

/* The component's own 64-bit id.  The lane's runtime-constant slot and the mask
   tool's per-pin memory are both keyed by it, because it survives reloads while
   the panel index does not. */
static uint64_t componentIdOf(const uint8_t* component) {
    uint64_t id = 0;
    memcpy(&id, component + 8, sizeof(id));
    return id;
}

/* The record of a board component by index, with its id and declared width: the
   mask layer needs both from a bare (board, index) pair, which is all the game's
   own get/set calls carry. */
static uint8_t* componentRecordAt(void* board, int64_t index, uint64_t* idOut, int* widthOut) {
    if (!board || index < 0) return nullptr;
    auto* bytes = static_cast<uint8_t*>(board);
    BoardView view{};
    view.board = board;
    memcpy(&view.count, bytes + 0x78, sizeof(view.count));
    memcpy(&view.data, bytes + 0x80, sizeof(view.data));
    if (!view.data || view.count == 0 || view.count > 1000000 ||
        static_cast<uint64_t>(index) >= view.count)
        return nullptr;
    uint8_t* component = componentAt(view, static_cast<uint64_t>(index));
    if (idOut) *idOut = componentIdOf(component);
    if (widthOut) *widthOut = componentWidth(component);
    return component;
}

/* The tape's geometry lives in tape_layout.hpp (tested offline); this file only
   decides how much room the drawer leaves and draws into it. */
using TapeLayout = tc_tape::Layout;
static constexpr int kBitsPerGroup = tc_tape::kBitsPerGroup;
static constexpr float kMaxScale = tc_tape::kMaxScale;
static constexpr float kMinScale = tc_tape::kMinScale;
static constexpr TapeLayout kPreferredLayout = tc_tape::kPreferred;
static constexpr float kHorizontalMargin = 16.f;
static constexpr float kVerticalMargin = 10.f;
/* The vanilla label and value inputs occupy the left side of the drawer.  Keep
   a separate column for them instead of letting the tape pass behind those
   already-drawn controls. */
static constexpr float kNativeControlsRight = 680.f;
static constexpr float kControlsTapeGap = 32.f;
/* The native drawer puts the heading at y=19, the value input at y=73 and the
   first visible sprite pixel at y=120.  The atlas tile has a half-pixel of
   transparent border at this scale, hence a canvas origin of 119.  It is a
   floor, not the whole layout: the free box below it decides the scale. */
static constexpr float kTapeTop = 119.f;

/* The driver that clicks the tape in the real-click playtest needs the canvas
   in screen (surface) space: the drawer's own window position is not published
   anywhere, so the canvas origin is the one anchor that turns a bit index into
   a mouse coordinate.  Reported only when it actually moves. */
static std::string lastGeometryReport;
static int geometryReports;
/* The drawer's presenter context, for the same report: a driver that has to ask
   that context for a recompile needs to know it is holding the same object. */
static const void* lastPresenterContext;

/* ------------------------------------------------------------- mask layer
   The mask is a *layer over a pin*, not a one-shot edit: the tape (and the
   game's own value field) keep showing and editing the punch data the player
   set, while the circuit is fed that data after the mask:

       effective = op(raw, mask)        op = AND v&m / OR v|m / XOR v^m /
                                            ANDNOT v&~m / 赋值 v=m

   Both halves come from one hook each, at the two calls the game itself uses:

     * reading a pin for the panel goes through get_component_global_input.  The
       plugin's hook already sits in that UI path, so it hands the panel the raw
       punch data while the game's own store keeps the transformed value.
     * every write - the native value field and the plugin alike - goes through
       set_component_global_input.  Hooking that turns "value = typed/clicked
       number" into "raw = that number, game gets op(raw, mask)".  A re-entrancy
       flag stops the plugin's own writes from being transformed twice.

   Constants use the two stores the wide-constant path already has: the
   component's setting keeps the punch data (what the drawer shows) and the
   runtime slot holds the transformed value the compiled circuit reads.

   The types, the bits, and a one-step undo are remembered per component id, so
   switching pins (or closing the window) does not lose them. */
/* The mask types a player picks between.  The label of each button spells the
   formula out, because "AND" and "ANDNOT" sitting next to each other is exactly
   where a mask tool gets misread (it did, in the field report: the AND the player
   wanted produced 256 because the neighbouring ANDNOT button was pressed). */
enum MaskOp { kMaskAnd = 0, kMaskOr, kMaskXor, kMaskAndNot, kMaskAssign, kMaskOpCount };
static constexpr uint32_t kMaskOpDefault = kMaskAnd;

struct MaskState {
    bool enabled = false;      /* the layer is active for this pin */
    uint32_t op = kMaskOpDefault;
    uint64_t mask = 0;         /* which bits the layer looks at */
    bool selecting = false;    /* clicks pick mask bits instead of data bits */
    /* The punch data the panel shows and edits.  The game's own store holds the
       transformed value while the layer is on. */
    bool hasRaw = false;
    uint64_t raw = 0;
    bool hasUndo = false;
    uint64_t undoRaw = 0;
};

struct MaskTarget {
    bool valid = false;
    uint64_t id = 0;
    int64_t index = -1;
    uint32_t width = 0;
    uint64_t value = 0;
    bool constant = false;
    void* board = nullptr;
};

static std::vector<std::pair<uint64_t, MaskState>> maskStates;
static MaskTarget maskTarget;
static char maskExpression[64] = "0x";
static const char* const kMaskPopupId = "punch_tape_mask_tool";
static std::vector<std::pair<uint64_t, uint64_t>> maskSelftestState;  /* id -> applied */
static std::string maskSelftestSpec;   /* op:expression, from the environment */
static bool maskSelftestDone;
static bool maskSelftestAwaiting;      /* the write landed; read it back next frame */
static uint64_t maskSelftestId;
static uint32_t maskSelftestWidth;
static int64_t maskSelftestIndex;

/* The loader's IO-value service, when this build has it: the mask tool then uses
   the same entry points the game's own value field does.  Without it the tool
   still works - writes fall back to the mod's own paths (flip per bit, or the
   constant's setting + runtime slot). */
static TCIoValueApiV1 ioValue;
static bool ioValueReady;
/* V2 of tc.pin_order is also the panel-entry layout broker.  This Mod produces
   bounds; it does not depend on the pin-order UI Mod being installed. */
static TCPinOrderApiV2 pinLayout;
static bool pinLayoutReady;

static MaskState& maskStateFor(uint64_t id) {
    for (auto& entry : maskStates)
        if (entry.first == id) return entry.second;
    maskStates.push_back(std::make_pair(id, MaskState{}));
    return maskStates.back().second;
}

/* Low `width` bits set; the truncation every game value field applies. */
static uint64_t valueMask(uint32_t width) {
    if (width == 0) return 0;
    if (width >= 64) return ~0ull;
    return (1ull << width) - 1ull;
}

static uint64_t maskApplyOp(uint64_t value, uint64_t mask, int op) {
    switch (op) {
        case kMaskAnd: return value & mask;      /* keep only the masked bits */
        case kMaskOr: return value | mask;       /* set the masked bits */
        case kMaskXor: return value ^ mask;      /* flip the masked bits */
        case kMaskAndNot: return value & ~mask;  /* clear the masked bits */
        case kMaskAssign: return mask;           /* value becomes the mask */
        default: return value;
    }
}

static const char* maskOpName(int op) {
    switch (op) {
        case kMaskAnd: return "and";
        case kMaskOr: return "or";
        case kMaskXor: return "xor";
        case kMaskAndNot: return "andnot";
        case kMaskAssign: return "assign";
        default: return "?";
    }
}

/* Button text: the name and the formula, so the two AND-ish entries cannot be
   confused again. */
static const char* maskOpLabel(int op) {
    switch (op) {
        case kMaskAnd: return "AND  v&m";
        case kMaskOr: return "OR  v|m";
        case kMaskXor: return "XOR  v^m";
        case kMaskAndNot: return "ANDNOT  v&~m";
        case kMaskAssign: return "赋值  v=m";
        default: return "?";
    }
}

static int maskOpFromName(const std::string& name) {
    if (name == "and" || name == "keep") return kMaskAnd;
    if (name == "or" || name == "set") return kMaskOr;
    if (name == "andnot" || name == "clear") return kMaskAndNot;
    if (name == "assign" || name == "replace") return kMaskAssign;
    if (name == "xor" || name == "toggle") return kMaskXor;
    return kMaskOpDefault;
}

static std::string formatValueText(uint64_t value, uint32_t width) {
    char text[80];
    if (ioValueReady && ioValue.format_value) {
        const int status = ioValue.format_value(ioValue.context, value, width,
                                                TC_IO_VALUE_FORMAT_HEX, text, sizeof(text));
        if (status == TC_IO_VALUE_OK || status == TC_IO_VALUE_ERR_SIZE) return text;
    }
    char digits[32];
    const uint32_t nibbles = width >= 64 ? 16u : (width + 3) / 4;
    std::snprintf(digits, sizeof(digits), "0x%0*llX", static_cast<int>(nibbles),
                  static_cast<unsigned long long>(value & valueMask(width)));
    return digits;
}

/* The same, but without the padding to the field's width: the mask button's
   label stays short enough for the sidebar. */
static std::string formatCompactHex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

static void reportMask(const std::string& message) {
    report("punch tape: " + message);
}

/* --------------------------------------------------- mask writes and actions
   An input's value goes through the game's own write when the loader exposes
   TC_SERVICE_IO_VALUE, and otherwise through the flip entry point the tape
   already uses (one call per bit that has to change).  A constant goes through
   write_constant (setting + runtime slot + refresh) or, on an older loader,
   through the same three steps the drawer hook has always used. */
static bool boardHandleOf(TCGameHandle* out) {
    return host && out && tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, out) == TC_HANDLE_OK;
}

static int popcount64(uint64_t value) {
    int count = 0;
    while (value) {
        value &= value - 1ull;
        ++count;
    }
    return count;
}

/* A select-mode click changes the mask only - never the value - and leaves a
   short record of what the player picked, so a bug report can show the mask the
   tool was holding (capped, like the geometry reports). */
static int maskBitReports;
static void toggleMaskBit(MaskState& state, int bit, uint32_t width, uint64_t componentId) {
    if (bit < 0 || bit >= 64) return;
    state.mask = (state.mask ^ (1ull << bit)) & valueMask(width);
    if (maskBitReports++ < 12)
        reportMask("mask bits now " + formatValueText(state.mask, width) + " (" +
                   std::to_string(popcount64(state.mask)) + " selected) on component " +
                   formatCompactHex(componentId));
}

/* The expression field: the loader's evaluator when there is one, a plain
   0x/0b/decimal literal otherwise (an older loader cannot parse expressions). */
static bool evaluateMaskExpression(const char* text, uint64_t* out) {
    if (!text || !out) return false;
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text) return false;
    if (ioValueReady && ioValue.evaluate &&
        ioValue.evaluate(ioValue.context, text, out) == TC_IO_VALUE_OK)
        return true;
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text += 2;
    }
    if (!*text) return false;
    uint64_t value = 0;
    for (const char* cursor = text; *cursor; ++cursor) {
        if (*cursor == '_') continue;
        int digit = -1;
        if (*cursor >= '0' && *cursor <= '9') digit = *cursor - '0';
        else if (*cursor >= 'a' && *cursor <= 'f') digit = *cursor - 'a' + 10;
        else if (*cursor >= 'A' && *cursor <= 'F') digit = *cursor - 'A' + 10;
        if (digit < 0 || digit >= base) return false;
        value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
    }
    *out = value;
    return true;
}

/* What the circuit sees for this pin: the punch data with the layer applied. */
static uint64_t effectiveValue(const MaskState& state, uint64_t raw, uint32_t width) {
    const uint64_t limit = valueMask(width);
    const uint64_t data = raw & limit;
    if (!state.enabled) return data;
    return maskApplyOp(data, state.mask & limit, static_cast<int>(state.op)) & limit;
}

/* True while the plugin itself is writing, so the set_component_global_input
   hook knows the value is already the effective one and must not be transformed
   a second time. */
static bool maskWriting;

/* Writes the effective value into the game's own store for this pin - the value
   the compiled circuit samples (measured on symphony_2_io: that store is the one
   a cycle reset consumes, which is the native behaviour of the game's own value
   field).  The panel does not read it while the layer is on: the getter hook
   hands it the punch data instead. */
static bool writeInputEffective(void* board, int64_t index, uint32_t width, uint64_t effective) {
    const uint64_t limit = valueMask(width);
    const uint64_t target = effective & limit;
    TCGameHandle handle{};
    if (ioValueReady && ioValue.write_input && ioValue.read_input && boardHandleOf(&handle)) {
        uint64_t current = 0;
        const bool known = ioValue.read_input(ioValue.context, &handle,
                                              static_cast<uint64_t>(index), &current) ==
                           TC_IO_VALUE_OK;
        if (known && (current & limit) == target) return true;
        maskWriting = true;
        const bool ok = ioValue.write_input(ioValue.context, &handle,
                                            static_cast<uint64_t>(index), target) == TC_IO_VALUE_OK;
        maskWriting = false;
        if (ok) return true;
    }
    /* No service: flip the bits that differ (the path a tape click always used). */
    if (!flipGlobalInput || !getGlobalInputOriginal) return false;
    const uint64_t current =
        static_cast<uint64_t>(getGlobalInputOriginal(board, index)) & limit;
    if (current == target) return true;
    const uint32_t bits = width < 64 ? width : 64;
    for (uint32_t bit = 0; bit < bits; ++bit)
        if (((current >> bit) & 1u) != ((target >> bit) & 1u))
            flipGlobalInput(board, index, static_cast<int64_t>(bit));
    return true;
}

/* The punch data changed (a tape click, the tool, or the native value field):
   remember it and hand the game the transformed value. */
static bool setInputRaw(void* board, int64_t index, uint64_t componentId, uint32_t width,
                        uint64_t raw, bool restorePrevious = true) {
    MaskState& state = maskStateFor(componentId);
    if (restorePrevious && state.hasRaw) {
        state.hasUndo = true;
        state.undoRaw = state.raw;
    }
    state.hasRaw = true;
    state.raw = raw & valueMask(width);
    return writeInputEffective(board, index, width, effectiveValue(state, state.raw, width));
}

/* Constants keep the punch data in the component's setting and the transformed
   value in the runtime slot the compiled circuit reads. */
static bool setConstantRaw(void* board, int64_t index, uint64_t componentId, uint32_t width,
                           uint64_t raw, bool restorePrevious = true) {
    MaskState& state = maskStateFor(componentId);
    if (restorePrevious && state.hasRaw) {
        state.hasUndo = true;
        state.undoRaw = state.raw;
    }
    state.hasRaw = true;
    state.raw = raw & valueMask(width);
    const uint64_t effective = effectiveValue(state, state.raw, width);
    if (setSetting) setSetting(board, kValueSetting, index, static_cast<int64_t>(state.raw));
    if (setDynamicConstant) {
        setDynamicConstant(componentId, effective);
    } else if (upgradeContext && lastPresenterContext) {
        auto* contextState =
            static_cast<uint8_t*>(const_cast<void*>(lastPresenterContext)) + kContextStateOffset;
        upgradeContext(contextState, kSettingsChangedState);
    }
    if (stopAndRefresh) stopAndRefresh(board);
    return setSetting != nullptr;
}

/* Re-applies the layer after the type, the bits or the enable flag changed. */
static bool refreshMaskTarget(bool restorePrevious = false) {
    if (!maskTarget.valid || !maskTarget.board) return false;
    MaskState& state = maskStateFor(maskTarget.id);
    if (!state.hasRaw) {
        state.hasRaw = true;
        state.raw = maskTarget.value & valueMask(maskTarget.width);
    }
    const uint64_t effective = effectiveValue(state, state.raw, maskTarget.width);
    const bool ok = maskTarget.constant
                        ? setConstantRaw(maskTarget.board, maskTarget.index, maskTarget.id,
                                         maskTarget.width, state.raw, restorePrevious)
                        : writeInputEffective(maskTarget.board, maskTarget.index,
                                              maskTarget.width, effective);
    if (ok) {
        maskTarget.value = state.raw;
        reportMask(std::string(maskOpName(state.op)) + (state.enabled ? " " : " off ") +
                   formatValueText(state.mask & valueMask(maskTarget.width), maskTarget.width) +
                   " on #" + std::to_string(maskTarget.index) + " width=" +
                   std::to_string(maskTarget.width) + " punch=" +
                   formatValueText(state.raw, maskTarget.width) + " circuit=" +
                   formatValueText(effective, maskTarget.width));
    } else {
        reportMask("mask write failed on #" + std::to_string(maskTarget.index));
    }
    return ok;
}

static void undoMaskAction() {
    if (!maskTarget.valid || !maskTarget.board) return;
    MaskState& state = maskStateFor(maskTarget.id);
    if (!state.hasUndo) return;
    const uint64_t restore = state.undoRaw & valueMask(maskTarget.width);
    const bool ok = maskTarget.constant
                        ? setConstantRaw(maskTarget.board, maskTarget.index, maskTarget.id,
                                         maskTarget.width, restore, false)
                        : setInputRaw(maskTarget.board, maskTarget.index, maskTarget.id,
                                      maskTarget.width, restore, false);
    if (!ok) {
        reportMask("mask undo failed on #" + std::to_string(maskTarget.index));
        return;
    }
    maskTarget.value = restore;
    state.hasUndo = false;
    reportMask("mask undo on #" + std::to_string(maskTarget.index) + " -> " +
               formatValueText(restore, maskTarget.width));
}

/* TC_MODLOADER_PUNCH_TAPE_MASK="<type>:<mask>[ @<punch data>]" switches the mask
   layer on for the first wide component a panel shows, optionally with given
   punch data, and then reports what the panel keeps, what the circuit gets and
   what the game's own read returns.  It is the same code path the tool window
   uses, so a sandbox run can exercise the layer without a human click. */
static void maskSelftest(void* board, uint64_t componentId, int64_t index, uint32_t width,
                         uint64_t value, bool constant) {
    if (maskSelftestSpec.empty() || !board) return;
    /* The game's own read catches up on the frame after a write (the native
       value field behaves the same way), so the second call reports. */
    if (maskSelftestAwaiting && componentId == maskSelftestId) {
        MaskState& state = maskStateFor(componentId);
        const uint64_t effective = effectiveValue(state, state.raw, width);
        uint64_t circuit = 0;
        bool circuitKnown = false;
        if (ioValueReady && ioValue.read_input) {
            TCGameHandle handle{};
            circuitKnown = boardHandleOf(&handle) &&
                           ioValue.read_input(ioValue.context, &handle,
                                              static_cast<uint64_t>(index), &circuit) ==
                               TC_IO_VALUE_OK;
        }
        reportMask("mask selftest layer " + std::string(maskOpName(state.op)) + " " +
                   formatValueText(state.mask, width) + " on #" + std::to_string(index) +
                   " width=" + std::to_string(width) +
                   " punch=" + formatValueText(state.raw, width) +
                   " panel=" + formatValueText(value & valueMask(width), width) +
                   " circuit=" + formatValueText(effective, width) +
                   " game-read=" + (circuitKnown ? formatValueText(circuit, width) : "?") +
                   " match=" +
                   (circuitKnown && (circuit & valueMask(width)) == effective ? "1" : "0") +
                   (ioValueReady ? " service=1" : " service=0"));
        maskSelftestAwaiting = false;
        return;
    }
    if (maskSelftestDone) return;
    maskSelftestDone = true;
    std::string spec = maskSelftestSpec;
    int op = kMaskOpDefault;
    const size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        op = maskOpFromName(spec.substr(0, colon));
        spec = spec.substr(colon + 1);
    }
    /* Optional "@<punch data>" so the test can put something in the tape first. */
    uint64_t raw = value & valueMask(width);
    const size_t at = spec.find('@');
    if (at != std::string::npos) {
        uint64_t parsed = 0;
        if (evaluateMaskExpression(spec.substr(at + 1).c_str(), &parsed))
            raw = parsed & valueMask(width);
        spec = spec.substr(0, at);
    }
    uint64_t mask = 0;
    if (!evaluateMaskExpression(spec.c_str(), &mask)) {
        reportMask("mask selftest could not read \"" + spec + "\"");
        return;
    }
    MaskState& state = maskStateFor(componentId);
    state.mask = mask & valueMask(width);
    state.op = static_cast<uint32_t>(op);
    state.enabled = true;
    maskTarget = MaskTarget{true, componentId, index, width, raw, constant, board};
    const uint64_t expected = effectiveValue(state, raw, width);
    const bool ok = constant ? setConstantRaw(board, index, componentId, width, raw, false)
                             : setInputRaw(board, index, componentId, width, raw, false);
    maskSelftestId = componentId;
    maskSelftestWidth = width;
    maskSelftestIndex = index;
    maskSelftestAwaiting = true;
    reportMask("mask selftest wrote layer " + std::string(maskOpName(op)) + " " +
               formatValueText(state.mask, width) + " on #" + std::to_string(index) +
               " width=" + std::to_string(width) + " punch=" + formatValueText(raw, width) +
               " expected=" + formatValueText(expected, width) + " layer=1 applied=" +
               (ok ? "1" : "0") +
               (ioValueReady ? " service=1" : " service=0"));
}

static void drawMaskPopup(MaskState& state, uint32_t width) {
    char line[224];
    const uint64_t limit = valueMask(width);
    if (!state.hasRaw) {
        state.hasRaw = true;
        state.raw = maskTarget.value & limit;
    }
    const uint64_t effective = effectiveValue(state, state.raw, width);
    std::snprintf(line, sizeof(line), "引脚 #%lld · %u 位 · %s",
                  static_cast<long long>(maskTarget.index), width,
                  maskTarget.constant ? "常量" : "输入");
    tc::ui::text(line);
    /* The two values side by side, which is the whole point of the layer. */
    std::snprintf(line, sizeof(line), "打孔数据（纸带/数值框）%s",
                  formatValueText(state.raw, width).c_str());
    tc::ui::text(line);
    std::snprintf(line, sizeof(line), "电路实际得到 %s%s",
                  formatValueText(effective, width).c_str(),
                  state.enabled ? "" : "   （掩码未启用，与打孔数据相同）");
    tc::ui::text(line);
    if (tc::ui::checkbox("启用掩码（纸带不变，只改电路拿到的值）", &state.enabled)) {
        if (state.enabled) {
            /* The punch data starts as whatever the pin currently holds. */
            state.hasRaw = true;
            state.raw = maskTarget.value & limit;
        }
        refreshMaskTarget();
    }
    tc::ui::separator();
    tc::ui::text("掩码类型:");
    for (uint32_t op = 0; op < kMaskOpCount; ++op) {
        if (op) tc::ui::sameLine();
        if (tc::ui::radioButton(maskOpLabel(static_cast<int>(op)), state.op == op)) {
            state.op = op;
            refreshMaskTarget();
        }
    }
    tc::ui::separator();
    /* The selection *is* the mask: bit n is selected exactly when the mask has a
       1 there.  Say so, because "select a bit" can also mean "select a bit of the
       value" and the tool does not touch the value while selecting. */
    std::snprintf(line, sizeof(line), "掩码 %s · 选中 %d 位（选中的位=掩码 1）",
                  formatValueText(state.mask & limit, width).c_str(),
                  popcount64(state.mask & limit));
    tc::ui::text(line);
    tc::ui::checkbox("选掩码位（只改掩码，不改数值）", &state.selecting);
    tc::ui::sameLine();
    if (tc::ui::button("全选")) {
        state.mask = limit;
        refreshMaskTarget();
    }
    tc::ui::sameLine();
    if (tc::ui::button("清空")) {
        state.mask = 0;
        refreshMaskTarget();
    }
    tc::ui::sameLine();
    if (tc::ui::button("反选")) {
        state.mask = (~state.mask) & limit;
        refreshMaskTarget();
    }
    tc::ui::separator();
    tc::ui::inputText("掩码表达式", maskExpression, sizeof(maskExpression));
    tc::ui::sameLine();
    if (tc::ui::button("作为掩码")) {
        uint64_t parsed = 0;
        if (evaluateMaskExpression(maskExpression, &parsed)) {
            state.mask = parsed & limit;
            refreshMaskTarget();
            reportMask("mask from expression \"" + std::string(maskExpression) + "\" = " +
                       formatValueText(state.mask, width) + " on #" +
                       std::to_string(maskTarget.index));
        } else {
            reportMask("mask expression \"" + std::string(maskExpression) + "\" not understood");
        }
    }
    if (!ioValueReady) {
        tc::ui::sameLine();
        tc::ui::textDisabled("(老加载器：仅字面量)");
    }
    tc::ui::separator();
    if (tc::ui::button("固化到打孔数据")) {
        /* Write what the circuit gets back into the punch data and drop the
           layer: useful once the mask has done its job. */
        const uint64_t baked = effectiveValue(state, state.raw, width);
        state.enabled = false;
        const bool ok = maskTarget.constant
                            ? setConstantRaw(maskTarget.board, maskTarget.index, maskTarget.id,
                                             width, baked)
                            : setInputRaw(maskTarget.board, maskTarget.index, maskTarget.id,
                                          width, baked);
        reportMask(std::string("mask baked ") + (ok ? "punch=" : "failed ") +
                   formatValueText(baked, width) + " on #" +
                   std::to_string(maskTarget.index));
    }
    tc::ui::sameLine();
    if (state.hasUndo) {
        if (tc::ui::button("撤销上一次")) undoMaskAction();
        tc::ui::sameLine();
    }
    if (tc::ui::button("关闭")) tc::ui::closeCurrentPopup();
}

/* The tape's own row of mask controls.  A compact button carries the current
   mask (so the panel always shows whether one is armed) and opens the popup;
   the sidebar adds the returned height to the room its entry has to make. */
static constexpr float kMaskRowHeight = 24.f;
static float drawMaskTool(void* board, int64_t index, uint64_t componentId, uint32_t width,
                          uint64_t value, bool constant) {
    MaskState& state = maskStateFor(componentId);
    const float startX = tc::ui::cursorPosX();
    const float startY = tc::ui::cursorPosY();
    char label[96];
    if (state.enabled)
        std::snprintf(label, sizeof(label), "掩码 ON %s %s", maskOpName(state.op),
                      formatCompactHex(state.mask).c_str());
    else if (state.mask)
        std::snprintf(label, sizeof(label), "掩码 %s %s (未启用)", maskOpName(state.op),
                      formatCompactHex(state.mask).c_str());
    else
        std::snprintf(label, sizeof(label), "掩码层…");
    if (tc::ui::button(label, {0, 0})) {
        maskTarget = MaskTarget{true, componentId, index, width, value, constant, board};
        tc::ui::openPopup(kMaskPopupId);
    }
    /* The row's height comes from the button the game just drew: its frame is
       taller than a bare text line, and the value field that follows must not
       land on top of it. */
    float rowHeight = tc::ui::itemRectSize().y + 6.f;
    if (rowHeight < kMaskRowHeight) rowHeight = kMaskRowHeight;
    if (state.selecting) {
        tc::ui::sameLine();
        tc::ui::textDisabled("选位中");
    }
    if (maskTarget.valid && maskTarget.id == componentId) {
        maskTarget.index = index;
        maskTarget.width = width;
        maskTarget.value = value;
        maskTarget.constant = constant;
        maskTarget.board = board;
        if (tc::ui::beginPopup(kMaskPopupId, 0)) {
            drawMaskPopup(state, width);
            tc::ui::endPopup();
        }
    }
    tc::ui::setCursorPos({startX, startY + rowHeight});
    return rowHeight;
}
/* Cell for bit `bit` of a `width`-bit value; false when the bit lies outside
   the drawn grid (it cannot, but the maths is kept total). */
static bool cellPositionForBit(int bit, int width, const TapeLayout& layout,
                               tc::ui::Vec2* out) {
    tc_tape::Point point;
    const tc_tape::Layout& box = layout;
    if (!tc_tape::cellPositionForBit(bit, width, box, &point)) return false;
    out->x = point.x;
    out->y = point.y;
    return true;
}

/* What the pointer asked for this frame: `clickedBit` is set once per physical
   left-button press, and the modifiers tell the caller which write that is -
   plain = flip, Shift = set, Ctrl = clear. */
struct TapeAction {
    int clickedBit = -1;
    bool shift = false;
    bool ctrl = false;
};

/* Draws the byte columns and reports what the pointer did.  Bits in `mask` carry
   a gold ring (the mask tool's selection), and the hovered cell shows its bit
   number and the current value inside the canvas, so the panel does not need a
   hint row. */
static bool drawGrid(const std::string& idPrefix, int width, int64_t value, uint64_t mask,
                     const TapeLayout& layout, TapeAction* action) {
    if (width < 1) width = 1;
    if (width > 64) width = 64;
    const float w = tc_tape::gridWidth(width, layout);
    const float h = tc_tape::gridHeight(width, layout);
    const float startX = tc::ui::cursorPosX();
    const float startY = tc::ui::cursorPosY();
    const bool drew = tc::ui::drawingReady();
    if (!drew) return false;

    using tc::ui::rgba;
    tc::ui::setCursorPos({startX, startY});
    const std::string canvasId = "cells_" + idPrefix;
    tc::ui::Canvas canvas(canvasId.c_str(), {w, h});
    if (!canvas) return false;
    {
        const tc::ui::Vec2 origin = canvas.origin();
        char context[32];
        std::snprintf(context, sizeof(context), " context=%llx",
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(lastPresenterContext)));
        char box[192];
        std::snprintf(box, sizeof(box),
                      " window=%dx%d grid=%dx%d scale=%d cell=%d",
                      static_cast<int>(tc::ui::windowWidth()),
                      static_cast<int>(tc::ui::windowHeight()),
                      static_cast<int>(tc_tape::gridWidth(width, layout)),
                      static_cast<int>(tc_tape::gridHeight(width, layout)),
                      static_cast<int>(layout.cell / kPreferredLayout.cell * 100.f),
                      static_cast<int>(layout.cell));
        const std::string geometry =
            "punch tape: geometry " + idPrefix + " origin=" +
            std::to_string(static_cast<int>(origin.x)) + "," +
            std::to_string(static_cast<int>(origin.y)) + " cell=" +
            std::to_string(static_cast<int>(layout.cell * 100.f)) + " gap=" +
            std::to_string(static_cast<int>(layout.cellGap * 100.f)) + " group=" +
            std::to_string(static_cast<int>(layout.groupGap * 100.f)) + " row=" +
            std::to_string(static_cast<int>(layout.rowGap * 100.f)) + " groups=" +
            std::to_string(layout.groupsPerRow) + " width=" + std::to_string(width) + context + box;
        if (geometry != lastGeometryReport && geometryReports++ < 8) {
            lastGeometryReport = geometry;
            report(geometry);
        }
    }
    const bool sprite = static_cast<bool>(chipTexture);

    /* Canvas is one ImGui item, so resolve its local mouse position back to a
       bit once and use that for both the native hover frame and clicking. */
    int hoveredBit = -1;
    const tc::ui::Vec2 mouse = canvas.mousePosition();
    if (canvas.hovered() || canvas.active()) {
        for (int bit = 0; bit < width; ++bit) {
            tc::ui::Vec2 at{};
            if (!cellPositionForBit(bit, width, layout, &at)) continue;
            if (mouse.x >= at.x && mouse.x < at.x + layout.cell &&
                mouse.y >= at.y && mouse.y < at.y + layout.cell) {
                hoveredBit = bit;
                break;
            }
        }
    }
    const bool pressed = hoveredBit >= 0 && tc::ui::isMouseDown(tc::ui::Mouse_Left);
    const bool shift = tc::ui::keys::down(tc::ui::Key_LeftShift) ||
                       tc::ui::keys::down(tc::ui::Key_RightShift);
    const bool ctrl = tc::ui::keys::down(tc::ui::Key_LeftCtrl) ||
                      tc::ui::keys::down(tc::ui::Key_RightCtrl);
    if (action) {
        action->shift = shift;
        action->ctrl = ctrl;
    }

    for (int bit = 0; bit < width; ++bit) {
        tc::ui::Vec2 at{};
        if (!cellPositionForBit(bit, width, layout, &at)) continue;
        const bool on = ((static_cast<uint64_t>(value) >> bit) & 1u) != 0;
        const bool masked = ((mask >> bit) & 1u) != 0;
        /* `far` would be the Windows legacy keyword, hence `corner`. */
        const tc::ui::Vec2 corner{at.x + layout.cell, at.y + layout.cell};
        const bool hovered = bit == hoveredBit;
        if (sprite) {
            /* The game's own chip: pink for a clear bit, green for a set one,
               including its original lighter hover/pressed frames. */
            const float x0 = kChipX0[on ? 1 : 0];
            const float y0 = kChipY[pressed && hovered ? 2 : (hovered ? 1 : 0)];
            const tc::ui::Vec2 uvMin{x0 / kSpriteWidth, y0 / kSpriteHeight};
            const tc::ui::Vec2 uvMax{(x0 + kChipSize) / kSpriteWidth,
                                     (y0 + kChipSize) / kSpriteHeight};
            chipTexture.draw(canvas, at, corner, uvMin, uvMax);
        } else {
            /* Fallback while the sprite is unavailable. */
            const uint32_t fill = hovered
                ? (on ? rgba(83, 204, 117) : rgba(246, 104, 126))
                : (on ? rgba(31, 177, 78) : rgba(236, 55, 85));
            canvas.rectFilled(at, corner, fill, layout.rounding);
            canvas.rect(at, corner, on ? rgba(24, 143, 62) : rgba(190, 42, 70),
                        layout.rounding, 1.f);
        }
        if (masked) {
            /* The mask tool's selection: a gold ring around the chip, drawn over
               whichever sprite frame the hover/press state picked. */
            canvas.rect({at.x - 1.f, at.y - 1.f}, {corner.x + 1.f, corner.y + 1.f},
                        rgba(245, 209, 82), layout.rounding + 1.f, 2.f);
        }
    }

    /* One cell click per physical button press.  The canvas' own "clicked" state
       is the game's InvisibleButton return value, which this engine reports on
       every frame the button is held - reading it directly made a single press
       flip the same bit dozens of times (the player saw the value flicker).  The
       press edge of the mouse is the reliable signal, and holding the button no
       longer repeats, which is what the tape wants. */
    static bool mouseWasDown;
    const bool mouseDown = tc::ui::isMouseDown(tc::ui::Mouse_Left);
    if (canvas.hovered()) {
        /* The edge belongs to the tape the pointer is on, so a panel with two
           tapes does not have the first one eat the other's press. */
        const bool pressedNow = mouseDown && !mouseWasDown;
        mouseWasDown = mouseDown;
        if (pressedNow && hoveredBit >= 0) {
            if (action) action->clickedBit = hoveredBit;
            tc::ui::setCursorPos({startX, startY + h + 6.f});
            return true;
        }
    } else if (!mouseDown) {
        mouseWasDown = false;
    }
    tc::ui::setCursorPos({startX, startY + h + 6.f});
    return false;
}

/* ------------------------------------------------------ workshop input hook
   The game calls get_component_global_input after drawing an input's label and
   immediately before choosing between its one-bit, 2..8-bit and wide-value
   controls.  At that exact call site the ImGui cursor is already at the native
   tape row.  Drawing here therefore preserves the game's labels and number
   editor and adds only the missing wide tape.

   The native row is deliberately compact: the bottom drawer's 0.25x scale
   floor is too large for the workshop side panel.  It may shrink to 0.04x,
   keeps byte groups intact, and uses the panel's current width for both its
   wrapping and scale. */
static constexpr float kWorkshopHorizontalMargin = 10.f;
/* The left edge of an entry is where the panel's own affordances live: the
   pin-order Mod draws its drag handle there (a frame at 6 px plus a grip strip
   the pointer can hit out to 25 px), so the tape and the mask row start past it
   instead of underneath it.  A tape that is centred across the whole width would
   otherwise reach back into that column as soon as it is nearly as wide as the
   panel. */
static constexpr float kEntryHandleInset = 20.f;
static constexpr float kWorkshopMinimumScale = 0.04f;
static constexpr float kWorkshopMaximumScale = 0.75f;
static constexpr float kWorkshopMinimumBand = 22.f;
static constexpr float kWorkshopMaximumBand = 48.f;
static constexpr float kWorkshopTallPanelThreshold = 430.f;
static constexpr float kWorkshopTallBandRatio = 0.23f;
static constexpr float kWorkshopTallMinimumBand = 80.f;
static constexpr float kWorkshopTallMaximumBand = 110.f;
static constexpr int kWorkshopGroupsPerRow = 2;
/* The gap drawGrid leaves between the canvas and the game's value box. */
static constexpr float kTapeBottomGap = 6.f;
/* Rows inside one workshop entry are a quarter of the holes: the drawer's 24 px
   row gap is three quarters of a 44 px cell, and carrying that into the narrow
   sidebar spent height that the entry does not have. */
static constexpr float kWorkshopRowGapRatio = 0.25f;
/* The drawer's byte-group gap is 28/44 of a cell, which is a lot of the little
   width a sidebar has; two fifths still separates the bytes and buys the holes
   a few percent. */
static constexpr float kWorkshopGroupGapRatio = 0.4f;

/* What the game's controls are measured against: the part of the child window
   the scrollbar does not cover.  Resolved separately because the SDK's UI table
   does not wrap them. */
using ContentRegionAvailFn = void (*)(tc::ui::Vec2*);
using ScrollMaxYFn = float (*)();
static ContentRegionAvailFn contentRegionAvail;
static ScrollMaxYFn scrollMaxY;
/* Debug switch, like the loader's TC_MODLOADER_TRACE_* ones: reserve this many
   pixels on the right as if the child had a scrollbar, so the path below can be
   looked at without a panel that happens to scroll. */
static float scrollbarReserve;

/* Width of the window that is not under the scrollbar.  ImGui puts the
   scrollbar inside the window, so a tape sized for the full width loses its
   last column as soon as the entries below it need the panel to scroll.  The
   content region is also inset by the window padding, and that inset alone is
   not worth shrinking the holes for, so the full width is kept while the window
   does not scroll at all. */
static float visibleWidth(float panelWidth) {
    const bool scrolled = (scrollMaxY && scrollMaxY() > 0.f) || scrollbarReserve > 0.f;
    if (!scrolled) return panelWidth;
    float right = panelWidth;
    if (contentRegionAvail) {
        tc::ui::Vec2 avail{};
        contentRegionAvail(&avail);
        /* GetContentRegionAvail measures from the cursor, so the cursor's X
           plus that width is the content region's right edge. */
        const float edge = tc::ui::cursorPosX() + avail.x;
        if (edge > 0.f && edge < right) right = edge;
    }
    if (scrollbarReserve > 0.f) {
        const float forced = panelWidth - scrollbarReserve;
        if (forced < right) right = forced;
    }
    if (right <= 0.f || right >= panelWidth) return panelWidth;
    return right;
}

/* ----------------------------------- workshop entry spacing (entries pushed down)
   The workshop's input list does not stack its entries: after every one the
   game anchors the next on its own fixed rhythm, held in a register, so an
   entry's height never moves what follows it.  The tape is drawn between an
   entry's label and the game's own value box, so a tape taller than the version
   of the entry the rhythm was sized for pushed that value box onto the next
   entry - the "输出状态" heading - instead of moving the next entry down.

   Shrinking the tape to the leftover is one answer, but it makes the holes
   small exactly where a wide value needs them, so this observer keeps the
   tape's size and moves everything below it instead.  Every igSetCursorPosY
   build_io_state_view makes is such an anchor: the two section headings'
   advances, the input loop's per-entry anchor, and the anchors the output lists
   use.  They are all absolute, so one offset covers them all - zero before the
   first tape, and from then on the room the tapes have taken.

   The amount comes from a measurement rather than from a guess: the native
   entry is label + value box and the rhythm leaves a fixed gap under it
   (measured on the 333 px sidebar: 121 px rhythm - 18 px label - 58 px box =
   45 px of gap).  Adding exactly the tape's height plus the canvas gap keeps
   that same gap under the taller entry, so an entry with a tape keeps the
   native spacing and the entries below it - including the whole outputs
   section - move down instead of being overlapped. */
using BeginChildFn = bool (*)(const char*, tc::ui::Vec2, int, int);
static BeginChildFn beginChildOriginal;
/* The window the panel's entry list lives in, captured while the panel's own
   child is open: the anchors below are accepted either because the call comes
   from build_io_state_view or because it is drawn inside that child.  The
   second test matters because not every line of the panel is placed by the
   panel's own code - the section headings come from a helper - and a line that
   is not moved keeps its old place while everything below it moves. */
static void* panelChildWindow = nullptr;
static void* (*getCurrentWindow)() = nullptr;
/* Return addresses immediately after build_io_state_view's calls, measured on
   this build: get_component_global_input (0x779, the tape hook above) and
   igBeginChild_Str (0x394, the panel starts here).  The anchors above are
   recognised by their address range instead, because the game anchors the
   outputs from its own code path: the symbol after build_io_state_view starts
   0x3fc0 bytes in, so every return address inside that window belongs to it. */
static constexpr uintptr_t kPanelChildCallReturn = 0x394;
static constexpr uintptr_t kIoStateViewSize = 0x3fc0;
/* Lowest priority: this Mod moves the anchors, so it has to run before a Mod
   that reads them (the pin-order Mod's handles follow the moved lines). */
static constexpr int32_t kAnchorPriority = 0;
static float slotOffset;          /* how far this panel's entries were pushed down */
static float slotPendingGrowth;   /* the tape just drawn in the current entry */
static int slotFrame;             /* frame the offset belongs to */
/* The tall component workshop first moves an absolute scalar Y, then repeats
   that already-moved value through igSetCursorPos for the label and bit rows.
   The compact in-level panel instead places rows directly with full positions. */
static bool slotScalarLayout;
static int slotReports;
static float slotReportedGrowth = -1.f;
struct PendingEntryBounds {
    bool valid = false;
    int32_t frame = -1;
    uint64_t key = 0;
    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
};
static PendingEntryBounds pendingEntryBounds;

static bool beginChildDetour(const char* id, tc::ui::Vec2 size, int childFlags,
                             int windowFlags) {
    if (ioStateView && __builtin_return_address(0) == ioStateView + kPanelChildCallReturn) {
        /* A fresh io state panel: its first entry sits at the natural rhythm. */
        slotOffset = 0.f;
        slotPendingGrowth = 0.f;
        slotScalarLayout = false;
        pendingEntryBounds.valid = false;
        slotFrame = tc::ui::frameCount();
        const bool opened =
            beginChildOriginal ? beginChildOriginal(id, size, childFlags, windowFlags) : false;
        panelChildWindow = getCurrentWindow ? getCurrentWindow() : nullptr;
        return opened;
    }
    return beginChildOriginal ? beginChildOriginal(id, size, childFlags, windowFlags) : false;
}

/* True when an address belongs to one specific PE image. */
static bool insideModule(const void* address, HMODULE module) {
    const auto base = reinterpret_cast<const unsigned char*>(module);
    if (!base || !address) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto at = reinterpret_cast<uintptr_t>(address);
    const auto begin = reinterpret_cast<uintptr_t>(base);
    return at >= begin && at < begin + nt->OptionalHeader.SizeOfImage;
}

/* The panel is split between the executable and helper code in the renamed
   original engine DLL.  In particular, some builds place the Outputs heading
   from tc_game_engine.dll.  Accept both game-owned images but not the proxy
   loader or a Mod DLL: a Mod's own cursor work must stay where that Mod put it. */
static bool insideGameModule(const void* address) {
    return insideModule(address, GetModuleHandleW(nullptr)) ||
           insideModule(address, GetModuleHandleW(L"tc_game_engine.dll"));
}

/* Every anchor inside the panel moves down by the room the tapes above have
   taken; a call from anywhere else is left alone.

   The panel anchors its entries two different ways and both of them have to
   move: the level panel positions an entry's label with igSetCursorPos (a whole
   position, so the Y in it is the panel's own, absolute), while the workshop
   panel anchors the same lines with igSetCursorPosY.  Watching only the scalar
   call is what used to leave a tape in a level panel sitting on top of the
   "outputs" heading: the heading moved, the entry that owned the tape did not.

   Both calls are loader hook chain points (several Mods need them: this one to
   make room, the pin-order Mod to draw its handles inside the same panel), so
   the panel is recognised by the caller the chain hands over. */
static bool panelAnchor(const TCHookCall* call, float* y, bool applyOffset) {
    if (!ioStateView) return false;
    const bool fromPanel = tc::hook::calledFrom(call, ioStateView, kIoStateViewSize);
    const bool inPanelWindow =
        panelChildWindow && getCurrentWindow && getCurrentWindow() == panelChildWindow &&
        insideGameModule(call->caller);
    if (!fromPanel && !inPanelWindow) return false;
    if (tc::ui::frameCount() != slotFrame) {
        slotFrame = tc::ui::frameCount();
        slotOffset = 0.f;
        slotPendingGrowth = 0.f;
    }
    const float growthBefore = slotPendingGrowth;
    if (slotPendingGrowth > 0.f) {
        /* This runs before the panel moves its cursor to the next absolute
           anchor, so cursorScreenPos is the true end of the value editor that
           followed the tape and mask controls.  Publish the completed entry
           to the loader; an outer decorator can now fit it even when this was
           the last entry and there is no next peer row to measure. */
        if (pinLayoutReady && pendingEntryBounds.valid &&
            pendingEntryBounds.frame == tc::ui::frameCount()) {
            const tc::ui::Vec2 end = tc::ui::cursorScreenPos();
            TCPinOrderBoundsV1 bounds{sizeof(bounds),TC_PIN_ORDER_BOUNDS_VERSION_1,
                pendingEntryBounds.frame,TC_PIN_ORDER_GROUP_INPUTS,pendingEntryBounds.key,
                pendingEntryBounds.minX,pendingEntryBounds.minY,pendingEntryBounds.maxX,
                std::max(end.y,pendingEntryBounds.minY)};
            const int status = tc::pin_order::includeBounds(pinLayout,bounds);
            if (status != TC_PIN_ORDER_OK && slotReports++ < 8)
                report("punch tape: entry bounds rejected (" + std::to_string(status) + ")");
        }
        pendingEntryBounds.valid = false;
        slotOffset += slotPendingGrowth;
        /* Report each size once (not once per frame), so a playtest can read the
           numbers back and compare them with a screenshot. */
        if (slotPendingGrowth != slotReportedGrowth && slotReports++ < 8) {
            slotReportedGrowth = slotPendingGrowth;
            report("punch tape: entry pushed down by " +
                   std::to_string(static_cast<int>(slotPendingGrowth)) + " px");
        }
        slotPendingGrowth = 0.f;
    }
    /* Development aid (TC_MODLOADER_PUNCH_TAPE_LOG=1): where each anchor came
       from, what it asked for and what it got.  A line the panel places from a
       helper shows up here as "window" instead of an offset inside the panel. */
    if (GetEnvironmentVariableW(L"TC_MODLOADER_PUNCH_TAPE_LOG",nullptr,0)>0 &&
        (growthBefore > 0.f || slotOffset > 0.f)) {
        static int seen=0;
        if(seen++<96){
            char line[192];
            char where[32];
            if(fromPanel)
                std::snprintf(where,sizeof(where),"+0x%llx",
                              static_cast<unsigned long long>(
                                  reinterpret_cast<uintptr_t>(call->caller)-
                                  reinterpret_cast<uintptr_t>(ioStateView)));
            else
                std::snprintf(where,sizeof(where),"window");
            std::snprintf(line,sizeof(line),
                          "punch tape: anchor %s asked=%.0f offset=%.0f got=%.0f",
                          where,static_cast<double>(*y),
                          static_cast<double>(slotOffset),
                          static_cast<double>(*y+(applyOffset?slotOffset:0.f)));
            report(line);
        }
    }
    if (applyOffset) *y += slotOffset;
    return true;
}

static int setCursorPosYLink(TCHookCall* call) {
    if (auto* args = tc::hook::setCursorPosYArgs(call)) panelAnchor(call, &args->y, true);
    return 0;
}

static int setCursorPosLink(TCHookCall* call) {
    if (auto* args = tc::hook::setCursorPosArgs(call))
        panelAnchor(call, &args->y, !slotScalarLayout);
    return 0;
}

static void tapeForWorkshopInput(void* board, int64_t index, int64_t value) {
    if (!board || index < 0 || !tc::ui::drawingReady()) return;
    BoardView view{};
    view.board = board;
    auto* bytes = static_cast<uint8_t*>(board);
    memcpy(&view.count, bytes + 0x78, sizeof(view.count));
    memcpy(&view.data, bytes + 0x80, sizeof(view.data));
    if (!view.data || view.count == 0 || view.count > 1000000 ||
        static_cast<uint64_t>(index) >= view.count)
        return;

    uint8_t* component = componentAt(view, static_cast<uint64_t>(index));
    const int width = componentWidth(component);
    /* The game already draws its own full native tape through eight bits. */
    if (width <= 8) return;

    ensureChipTexture();
    const float panelWidth = tc::ui::windowWidth();
    const float panelHeight = tc::ui::windowHeight();
    slotScalarLayout = panelHeight > kWorkshopTallPanelThreshold;
    /* The scrollbar lives inside the window, so the tape is measured and centred
       against the visible part of it - otherwise the last byte group ends up
       underneath the bar as soon as the panel scrolls. */
    const float usableWidth = visibleWidth(panelWidth);
    /* The tape is centred between the entry's handle column and the right
       margin, not across the whole window. */
    const float leftEdge = kWorkshopHorizontalMargin + kEntryHandleInset;
    const float availableWidth = usableWidth - leftEdge - kWorkshopHorizontalMargin;
    if (availableWidth <= 0.f) return;
    /* How much room the tape gets.  This no longer has to fit the entry's own
       leftover: whatever the tape takes, the entry below it is pushed down by
       the same amount (see the anchor observer above), so the sidebar can spend
       the panel's height on the holes the way the bottom drawer does. */
    float availableHeight = panelWidth * 0.12f;
    if (availableHeight < kWorkshopMinimumBand) availableHeight = kWorkshopMinimumBand;
    if (availableHeight > kWorkshopMaximumBand) availableHeight = kWorkshopMaximumBand;
    /* The component workshop gives this child substantially more vertical room
       than the compact in-level IO panel, and that room now goes into bigger
       holes rather than into the gap that follows them. */
    if (panelHeight > kWorkshopTallPanelThreshold) {
        availableHeight = panelHeight * kWorkshopTallBandRatio;
        if (availableHeight < kWorkshopTallMinimumBand)
            availableHeight = kWorkshopTallMinimumBand;
        if (availableHeight > kWorkshopTallMaximumBand)
            availableHeight = kWorkshopTallMaximumBand;
    }

    /* Rows keep the compact quarter-cell gap of the entries they sit in, and the
       layout is told about it so the fit spends the whole band on hole size
       instead of reserving the drawer's much looser 24 px rows. */
    TapeLayout preferred = tc_tape::kPreferred;
    preferred.rowGap = preferred.cell * kWorkshopRowGapRatio;
    preferred.groupGap = preferred.cell * kWorkshopGroupGapRatio;
    TapeLayout layout = tc_tape::makeLayout(
        width, availableWidth, availableHeight,
        kWorkshopMinimumScale, kWorkshopMaximumScale,
        kWorkshopGroupsPerRow, preferred);
    const float gridW = tc_tape::gridWidth(width, layout);
    const float gridH = tc_tape::gridHeight(width, layout);
    /* What this entry needs on top of the game's own rhythm: the tape itself
       plus the gap drawGrid leaves under it.  The anchor observer adds it to
       the next entry's Y, so the native spacing below the value box survives. */
    slotPendingGrowth = gridH + kTapeBottomGap;
    float tapeX = leftEdge + (usableWidth - leftEdge - kWorkshopHorizontalMargin - gridW) * 0.5f;
    if (tapeX < leftEdge) tapeX = leftEdge;
    const float rightmostX = usableWidth - kWorkshopHorizontalMargin - gridW;
    if (tapeX > rightmostX) tapeX = rightmostX;
    float tapeY = tc::ui::cursorPosY();
    /* Keep the last row inside the child window even at unusually short window
       sizes.  The numeric editor follows the tape and remains clipped by the
       same native child, exactly like the rest of the panel. */
    if (panelHeight > 0.f && tapeY + gridH > panelHeight - 4.f) {
        const float lifted = panelHeight - 4.f - gridH;
        if (lifted >= 0.f) tapeY = lifted;
    }
    tc::ui::setCursorPos({tapeX, tapeY});
    const tc::ui::Vec2 tapeScreen = tc::ui::cursorScreenPos();
    pendingEntryBounds = {true,tc::ui::frameCount(),static_cast<uint64_t>(index),
                          tapeScreen.x,tapeScreen.y,tapeScreen.x + gridW};
    const uint64_t componentId = componentIdOf(component);
    MaskState& state = maskStateFor(componentId);
    const uint32_t valueWidth = static_cast<uint32_t>(width);
    uint64_t current = static_cast<uint64_t>(value) & valueMask(valueWidth);
    TapeAction action{};
    drawGrid("workshop_input" + std::to_string(index), width, value, state.mask, layout,
             &action);
    /* The tool acts on this entry, and its base value follows the panel's value
       (except for the writes the tool itself makes). */
    maskTarget = MaskTarget{true, componentId, index, valueWidth, current, false, board};
    if (!state.hasRaw) {
        state.hasRaw = true;
        state.raw = current;
    }
    /* A data click.  With the layer off it keeps the tape's own behaviour (one
       native flip, or set/clear under Shift/Ctrl); with the layer on the punch
       data changes and the circuit is handed the transformed value instead. */
    auto editData = [&](int bit, int mode) {
        uint64_t raw = state.hasRaw ? state.raw : current;
        if (mode == 0) raw ^= 1ull << bit;
        else if (mode == 1) raw |= 1ull << bit;
        else raw &= ~(1ull << bit);
        raw &= valueMask(valueWidth);
        if (state.enabled) {
            if (!setInputRaw(board, index, componentId, valueWidth, raw))
                reportMask("mask data write failed on #" + std::to_string(index));
        } else {
            const bool before = ((current >> bit) & 1u) != 0;
            const bool after = ((raw >> bit) & 1u) != 0;
            if (before != after && flipGlobalInput) flipGlobalInput(board, index, bit);
            state.hasRaw = true;
            state.raw = raw;
        }
        current = raw;
        maskTarget.value = raw;
        report("punch tape: workshop input #" + std::to_string(index) + " bit " +
               std::to_string(bit) +
               (mode == 0 ? " flipped" : (mode == 1 ? " set" : " cleared")) +
               (state.enabled ? " (mask layer on)" : ""));
    };
    /* One cell the pointer touched: select mode only edits the mask, otherwise
       the modifiers pick between set, clear and the tape's own flip. */
    auto actOnBit = [&](int bit) {
        if (bit < 0 || bit >= width) return;
        if (state.selecting) {
            toggleMaskBit(state, bit, valueWidth, componentId);
            refreshMaskTarget();
            current = state.raw;
            maskTarget.value = current;
            return;
        }
        editData(bit, action.shift ? 1 : (action.ctrl ? 2 : 0));
    };
    if (action.clickedBit >= 0) actOnBit(action.clickedBit);
    /* The mask row sits between the tape and the game's value field, so the
       entry asks for that much more room from the anchor observer. */
    const float maskRow = drawMaskTool(board, index, componentId, valueWidth, current, false);
    slotPendingGrowth = gridH + kTapeBottomGap + maskRow;
    maskSelftest(board, componentId, index, valueWidth, current, false);
    /* drawGrid leaves the cursor immediately below the tape.  The game's wide
       number editor is drawn next, so it naturally occupies the same place as
       the value text under the original 2..8-bit tape. */
}

static int64_t getGlobalInputDetour(void* board, int64_t component) {
    void* caller = __builtin_return_address(0);
    int64_t value = getGlobalInputOriginal ? getGlobalInputOriginal(board, component) : 0;
    if (ioStateView && caller == ioStateView + kIoInputValueCallReturn) {
        /* The panel shows the punch data.  The game's own store holds what the
           circuit reads, which with the layer on is the transformed value; only
           while the layer is active does this override, so a pin without a mask
           behaves exactly as before. */
        uint64_t id = 0;
        int width = 8;
        if (componentRecordAt(board, component, &id, &width)) {
            MaskState& state = maskStateFor(id);
            if (state.enabled) {
                if (!state.hasRaw) {
                    state.hasRaw = true;
                    state.raw = static_cast<uint64_t>(value) & valueMask(static_cast<uint32_t>(width));
                }
                value = static_cast<int64_t>(state.raw);
            }
        }
        tapeForWorkshopInput(board, component, value);
    }
    return value;
}

/* The game's write path for a pin's value: the native value field and the
   plugin's own writes both land here.  While the layer is on, what arrives is
   the punch data, so it is remembered and the game is handed the transformed
   value instead.  The re-entrancy flag keeps the plugin's own writes (which
   already carry the effective value) from being transformed twice. */
static void setGlobalInputDetour(void* board, int64_t component, int64_t value) {
    uint64_t id = 0;
    int width = 8;
    if (!maskWriting && componentRecordAt(board, component, &id, &width)) {
        MaskState& state = maskStateFor(id);
        if (state.enabled) {
            const uint32_t bits = static_cast<uint32_t>(width);
            const uint64_t raw = static_cast<uint64_t>(value) & valueMask(bits);
            state.hasRaw = true;
            state.raw = raw;
            maskWriting = true;
            if (setGlobalInputOriginal)
                setGlobalInputOriginal(
                    board, component,
                    static_cast<int64_t>(effectiveValue(state, raw, bits)));
            maskWriting = false;
            return;
        }
    }
    if (setGlobalInputOriginal) setGlobalInputOriginal(board, component, value);
}

/* --------------------------------------------------------------- drawer hook
   Called from inside the component drawer, after the game has drawn its own
   content, so the squares appear in that panel and not in a separate window. */
static void tapeForComponent(int64_t index, int64_t wordSize, void* presenterContext) {
    const BoardView view = boardView();
    ensureChipTexture();
    if (!view.board || index < 0 || static_cast<uint64_t>(index) >= view.count) return;
    uint8_t* component = componentAt(view, index);
    if (componentKind(component) != kConstantKind || componentSettingCount(component) <= 0) return;

    /* The component's own bit width, not the level's: a 64-bit constant on an
       8-bit board still has 64 bits. */
    int64_t width = 8;
    if (auto* sizePtr = static_cast<const uint64_t*>(
            host->resolve_symbol(host->context, "current_word_size__modelZmodel95types_u741"))) {
        if (*sizePtr >= 1 && *sizePtr <= 64) width = static_cast<int64_t>(*sizePtr);
    }
    width = componentWidth(component, static_cast<int>(width));
    (void)wordSize;
    /* Vanilla already supplies its native tape for narrow constants. */
    if (width <= 8) return;

    int64_t value = 0;
    if (!componentSetting(component, kValueSetting, &value)) return;
    lastPresenterContext = presenterContext;

    /* The native label/value controls have already been drawn on the left.
       Reserve that column and centre the tape when the normal centred position
       is farther right; otherwise begin just after the controls.

       Both room measurements come from the drawer itself, so the tape follows
       the window: a wider panel grows the chips (up to kMaxScale) and a wider
       one wraps whole byte groups first; a shorter one scales them down until
       the last row still fits under the heading. */
    const float panelWidth = tc::ui::windowWidth();
    const float panelHeight = tc::ui::windowHeight();
    float tapeLeft = kNativeControlsRight + kControlsTapeGap;
    if (tapeLeft > panelWidth * 0.45f) tapeLeft = panelWidth * 0.45f;
    const float tapeAreaWidth = panelWidth - tapeLeft - kHorizontalMargin;
    const float tapeAreaHeight =
        panelHeight > 0.f ? panelHeight - kTapeTop - kVerticalMargin : -1.f;
    const TapeLayout layout =
        tc_tape::makeLayout(static_cast<int>(width), tapeAreaWidth, tapeAreaHeight);
    const float gridW = tc_tape::gridWidth(static_cast<int>(width), layout);
    const float gridH = tc_tape::gridHeight(static_cast<int>(width), layout);
    float centeredX = (panelWidth - gridW) * 0.5f;
    if (centeredX < tapeLeft) centeredX = tapeLeft;
    const float rightmostX = panelWidth - kHorizontalMargin - gridW;
    if (centeredX > rightmostX) centeredX = rightmostX;
    if (centeredX < 8.f) centeredX = 8.f;
    const float originalX = tc::ui::cursorPosX();
    const float originalY = tc::ui::cursorPosY();
    float tapeY = kTapeTop;
    /* A drawer too short for the preferred rhythm: lift the tape as far as the
       heading allows before accepting a clipped last row. */
    if (panelHeight > 0.f && tapeY + gridH > panelHeight - kVerticalMargin) {
        const float lifted = panelHeight - kVerticalMargin - gridH;
        tapeY = lifted > 4.f ? lifted : 4.f;
    }
    tc::ui::setCursorPos({centeredX, tapeY});
    if (layoutReports++ < 3)
        report("punch tape: constant #" + std::to_string(index) + " width=" +
               std::to_string(width) + " panel=" +
               std::to_string(static_cast<int>(panelWidth)) + " original=" +
               std::to_string(static_cast<int>(originalX)) + "," +
               std::to_string(static_cast<int>(originalY)) + " tape=" +
               std::to_string(static_cast<int>(centeredX)) + "," +
               std::to_string(static_cast<int>(tapeY)));
    int64_t pendingValue = value;
    const uint64_t componentId = componentIdOf(component);
    const uint32_t valueWidth = static_cast<uint32_t>(width);
    MaskState& state = maskStateFor(componentId);
    uint64_t current = static_cast<uint64_t>(value) & valueMask(valueWidth);
    TapeAction action{};
    drawGrid("drawer" + std::to_string(index), static_cast<int>(width), value, state.mask,
             layout, &action);
    maskTarget = MaskTarget{true, componentId, index, valueWidth, current, true, view.board};
    if (!state.hasRaw) {
        state.hasRaw = true;
        state.raw = current;
    }
    /* One cell the pointer touched.  With the layer off this is the drawer's own
       write path (setting + runtime slot + refresh); with it on, the setting
       keeps the punch data and the runtime slot carries the transformed value. */
    auto editData = [&](int bit, int mode) {
        uint64_t raw = state.hasRaw ? state.raw : current;
        if (mode == 0) raw ^= 1ull << bit;
        else if (mode == 1) raw |= 1ull << bit;
        else raw &= ~(1ull << bit);
        raw &= valueMask(valueWidth);
        if (state.enabled) {
            if (!setConstantRaw(view.board, index, componentId, valueWidth, raw))
                reportMask("mask data write failed on #" + std::to_string(index));
        } else {
            memcpy(&pendingValue, &raw, sizeof(pendingValue));
            if (setSetting) {
                setSetting(view.board, kValueSetting, index, pendingValue);
                if (setDynamicConstant) {
                    setDynamicConstant(componentId, raw);
                } else if (upgradeContext && presenterContext) {
                    auto* contextState =
                        static_cast<uint8_t*>(presenterContext) + kContextStateOffset;
                    upgradeContext(contextState, kSettingsChangedState);
                }
                if (stopAndRefresh) stopAndRefresh(view.board);
            }
            state.hasRaw = true;
            state.raw = raw;
        }
        current = raw;
        maskTarget.value = raw;
        report("punch tape: drawer component #" + std::to_string(index) + " bit " +
               std::to_string(bit) +
               (mode == 0 ? " -> value " : (mode == 1 ? " set -> " : " cleared -> ")) +
               std::to_string(raw) + (state.enabled ? " (mask layer on)" : ""));
    };
    auto actOnBit = [&](int bit) {
        if (bit < 0 || bit >= width) return;
        if (state.selecting) {
            toggleMaskBit(state, bit, valueWidth, componentId);
            refreshMaskTarget();
            current = state.raw;
            maskTarget.value = current;
            return;
        }
        editData(bit, action.shift ? 1 : (action.ctrl ? 2 : 0));
    };
    if (action.clickedBit >= 0) actOnBit(action.clickedBit);
    drawMaskTool(view.board, index, componentId, valueWidth, current, true);
    maskSelftest(view.board, componentId, index, valueWidth, current, true);
}

static void panelDetour(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    if (panelOriginal) panelOriginal(a, b, c, d, e, f, g, h);
    if (!panelSeen) {
        panelSeen = true;
        report("punch tape: component drawer hooked");
    }
    /* d is the game's pointer to the component index the drawer is showing. */
    int64_t index = -1;
    if (d) index = *static_cast<const int64_t*>(d);
    int64_t wordSize = 8;
    if (auto* sizePtr = static_cast<const uint64_t*>(
            host->resolve_symbol(host->context, "current_word_size__modelZmodel95types_u741"))) {
        if (*sizePtr >= 1 && *sizePtr <= 64) wordSize = static_cast<int64_t>(*sizePtr);
    }
    tapeForComponent(index, wordSize, b);
    (void)e;
    (void)f;
    (void)g;
    (void)h;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::hostHas(h, TC_CAP_GAME_HANDLES) || !h->resolve_symbol || !h->create_hook) return 2;
    if (!tc::ui::load(h)) {
        report("punch tape: tc::ui::load failed: " + tc::ui::missing());
        return 3;
    }
    if (!tc::ui::loadDrawing(h)) report("punch tape: drawing unavailable");
    /* The game's own chip sprite is loaded from the installation at run time;
       without the texture API (older loaders) the panel falls back to drawn
       chips of the same colour. */
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!tc::ui::loadTextures(h)) report("punch tape: texture API unavailable; drawn chips");
    setSetting = reinterpret_cast<SetSettingFn>(h->resolve_symbol(
        h->context, "set_setting__presenterZutilitiesZhelper95functions_u2763"));
    upgradeContext = reinterpret_cast<UpgradeContextFn>(h->resolve_symbol(
        h->context, "upgrade__presenterZcontext_u2766"));
    stopAndRefresh = reinterpret_cast<StopAndRefreshFn>(h->resolve_symbol(
        h->context, "sim_stop_and_refresh__modelZsimulationZcompile95thread_u3043"));
    flipGlobalInput = reinterpret_cast<FlipGlobalInputFn>(h->resolve_symbol(
        h->context,
        "flip_component_global_input__presenterZutilitiesZhelper95functions_u5846"));
    ioStateView = static_cast<uint8_t*>(h->resolve_symbol(
        h->context,
        "build_io_state_view__presenterZboard95uiZio95state95view_u100"));
    /* engine_proc intentionally exposes the renamed original engine module.
       This bridge belongs to the proxy loader itself, whose process module is
       still named game_engine.dll. */
    HMODULE loaderModule = GetModuleHandleW(L"game_engine.dll");
    FARPROC dynamicConstantProc =
        loaderModule ? GetProcAddress(loaderModule, "tc_dynamic_constant_set") : nullptr;
    static_assert(sizeof(setDynamicConstant) == sizeof(dynamicConstantProc));
    memcpy(&setDynamicConstant, &dynamicConstantProc, sizeof(setDynamicConstant));
    if (!setSetting || !upgradeContext || !stopAndRefresh || !flipGlobalInput ||
        !ioStateView) {
        report("punch tape: game entry point missing (set_setting/upgrade/refresh/workshop input)");
        return 4;
    }
    report(setDynamicConstant ? "punch tape: runtime wide constants enabled"
                              : "punch tape: runtime constants unavailable; compile fallback");
    /* The squares live in the component drawer itself, under the fields the
       drawer already shows: no separate panel. */
    void* drawer = h->resolve_symbol(
        h->context,
        "build_component_description_panel__presenterZboard95uiZbottom95panelZcomponent95description_u1227");
    if (!drawer) {
        report("punch tape: component drawer symbol not found");
        return 5;
    }
    void* getGlobalInput = h->resolve_symbol(
        h->context,
        "get_component_global_input__presenterZutilitiesZhelper95functions_u9752");
    if (!getGlobalInput) {
        report("punch tape: workshop input value symbol not found");
        return 6;
    }
    const int inputHook = h->create_hook(
        h->context, getGlobalInput,
        reinterpret_cast<void*>(&getGlobalInputDetour),
        reinterpret_cast<void**>(&getGlobalInputOriginal));
    if (inputHook != 0) {
        report("punch tape: workshop input hook refused (" +
               std::to_string(inputHook) + ")");
        return 7;
    }
    /* The mask layer needs the write side too: whatever changes a pin's value
       (the native value field included) has to be seen as new punch data. */
    void* setGlobalInput = h->resolve_symbol(
        h->context,
        "set_component_global_input__presenterZutilitiesZhelper95functions_u5857");
    if (!setGlobalInput) {
        report("punch tape: input write symbol not found; mask layer disabled");
    } else {
        const int writeHook = h->create_hook(
            h->context, setGlobalInput,
            reinterpret_cast<void*>(&setGlobalInputDetour),
            reinterpret_cast<void**>(&setGlobalInputOriginal));
        if (writeHook != 0)
            report("punch tape: input write hook refused (" + std::to_string(writeHook) +
                   "); mask layer disabled");
        else
            report("punch tape: mask layer installed (punch data / circuit value)");
    }
    const int hook = h->create_hook(h->context, drawer, reinterpret_cast<void*>(&panelDetour),
                                    reinterpret_cast<void**>(&panelOriginal));
    if (hook != 0) {
        report("punch tape: component drawer hook refused (" + std::to_string(hook) + ")");
        return 8;
    }
    /* Two tiny observers that let a tall tape push the entries below it down
       instead of being squeezed into the game's own entry rhythm: the panel's
       igBeginChild_Str marks where the entry list starts, and the input loop's
       igSetCursorPosY is where the next entry (and, after the last one, the
       outputs heading) is anchored.  Both forward untouched. */
    void* panelChild = h->resolve_symbol(h->context, "igBeginChild_Str");
    if (!panelChild && h->engine_proc)
        panelChild = h->engine_proc(h->context, "igBeginChild_Str");
    void* regionAvail = h->resolve_symbol(h->context, "igGetContentRegionAvail");
    if (!regionAvail && h->engine_proc)
        regionAvail = h->engine_proc(h->context, "igGetContentRegionAvail");
    /* The panel's entry list window: while it is the current window, a line
       drawn inside it belongs to the panel even when the call was made from a
       helper rather than from build_io_state_view itself. */
    void* currentWindow = h->resolve_symbol(h->context, "igGetCurrentWindow");
    if (!currentWindow && h->engine_proc)
        currentWindow = h->engine_proc(h->context, "igGetCurrentWindow");
    if (currentWindow) {
        static_assert(sizeof(getCurrentWindow) == sizeof(currentWindow));
        memcpy(&getCurrentWindow, &currentWindow, sizeof(getCurrentWindow));
    } else {
        report("punch tape: current-window lookup unavailable; only the panel's own lines move");
    }
    void* scrollRange = h->resolve_symbol(h->context, "igGetScrollMaxY");
    if (!scrollRange && h->engine_proc)
        scrollRange = h->engine_proc(h->context, "igGetScrollMaxY");
    if (regionAvail) {
        static_assert(sizeof(contentRegionAvail) == sizeof(regionAvail));
        memcpy(&contentRegionAvail, &regionAvail, sizeof(contentRegionAvail));
    } else {
        report("punch tape: content region unavailable; tape ignores the scrollbar");
    }
    if (scrollRange) {
        static_assert(sizeof(scrollMaxY) == sizeof(scrollRange));
        memcpy(&scrollMaxY, &scrollRange, sizeof(scrollMaxY));
    }
    /* The mask tool prefers the loader's IO-value service: same entry points the
       game's own value field uses, so an expression typed into the tool behaves
       like one typed into the field.  Without it the tool still works (the
       popup says so, and writes fall back to the mod's own paths). */
    if (tc::ioValueService(h, &ioValue) == TC_SERVICE_OK && tc::io_value::ready(ioValue)) {
        ioValueReady = true;
        report("punch tape: IO value service available (mask expressions)");
    } else {
        ioValueReady = false;
        ioValue = TCIoValueApiV1{};
        report("punch tape: IO value service unavailable; mask tool uses literals");
    }
    pinLayoutReady = tc::pin_order::table(h, &pinLayout) && tc::pin_order::ready(pinLayout);
    report(pinLayoutReady ? "punch tape: pin entry bounds are reported through tc.pin_order V2"
                          : "punch tape: pin entry bounds unavailable; using anchor spacing only");
    {
        char spec[96]{};
        if (GetEnvironmentVariableA("TC_MODLOADER_PUNCH_TAPE_MASK", spec, sizeof(spec)) > 0) {
            maskSelftestSpec = spec;
            report("punch tape: mask self-test armed (" + maskSelftestSpec + ")");
        }
    }
    wchar_t reserveText[32]{};
    if (GetEnvironmentVariableW(L"TC_MODLOADER_PUNCH_TAPE_SCROLLBAR", reserveText, 32) > 0) {
        scrollbarReserve = static_cast<float>(_wtoi(reserveText));
        if (scrollbarReserve > 0.f)
            report("punch tape: reserving " +
                   std::to_string(static_cast<int>(scrollbarReserve)) +
                   " px as a scrollbar (TC_MODLOADER_PUNCH_TAPE_SCROLLBAR)");
    }
    if (panelChild) {
        const int childHook = h->create_hook(
            h->context, panelChild, reinterpret_cast<void*>(&beginChildDetour),
            reinterpret_cast<void**>(&beginChildOriginal));
        /* The two anchors are loader hook chain points, so this Mod joins them
           instead of hooking them: the pin-order Mod draws inside the same panel
           and both have to see every call. */
        const int lineLink =
            tc::hook::addSetCursorPosY(h, kAnchorPriority, &setCursorPosYLink, nullptr);
        const int entryLink =
            tc::hook::addSetCursorPos(h, kAnchorPriority, &setCursorPosLink, nullptr);
        if (childHook != 0 || lineLink != TC_HOOK_OK || entryLink != TC_HOOK_OK) {
            report("punch tape: entry spacing hooks refused (" +
                   std::to_string(childHook) + "/" + std::to_string(lineLink) + "/" +
                   std::to_string(entryLink) + ")");
        } else {
            report("punch tape: entries make room for the tape (level and workshop)");
        }
    } else {
        report("punch tape: entry spacing hooks unavailable");
    }
    report("punch tape: workshop input hook installed");
    report("punch tape: constant drawer hook installed");
    return 0;
}
