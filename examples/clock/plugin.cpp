/* The clock source the gate-delay mode is meant to be tested with.

   The game's own sources move at cycle boundaries only (a manual button, a
   switch, `Time`), which is exactly what the unit-based delay model is not: a
   circuit clocked that way shows nothing of the propagation the mode adds.  So
   this Mod registers a pure source - no inputs, one 1-bit output - and the
   delay model turns it into a unit-accurate one:

     mode off (or a normal level): the callback below flips the output once per
     cycle, which is an ordinary square wave for any other use.

     mode on (sandbox): reads of this component's value slot answer with a
     one-unit pulse at the start of every cycle (src/gate_delay.hpp, "the clock
     source"), and that pulse then travels through the delay model like any
     other signal: it costs a unit per gate, it can be caught by a gate-built
     latch, and a pulse narrower than the path delay can be missed - which is the
     whole point of having it.

   One cycle per flip is only the default.  The period is the number in the
   small box on the component's top-left corner - the same "click the number,
   type a value" affordance the game's own adjustable parts have: clicking the
   box opens a small value window (the game's own ImGui, so it looks and types
   like the game's), and what the player enters is parsed by the game's own
   expression evaluator, so `8`, `0x10` and `1+7` all mean what they mean in a
   value field.  That makes a slow clock for watching propagation, or a
   divide-by-N beat for a counter, a click and a number away.

   The setting travels with the schematic: the period is one byte of the
   instance's own configuration, the same host-owned blob the switch keeps its
   level in, so it survives saving, and a board saved before the field existed
   comes back on the registered default of one cycle.

   The custom id is fixed and shared with the loader (kClockPrototypeId). */

#include "../../sdk/tc_mod.h"
#include "../../sdk/tc_component_types.h"
#include "../../sdk/tc_board_model.h"
#include "../../sdk/tc_component_geometry.h"
#include "../../sdk/tc_component_instances.h"
#include "../../sdk/tc_component_render.h"
#include "../../sdk/tc_component_storage.h"
#include "../../sdk/tc_io_value.h"
#include "../../sdk/tc_ui.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <mutex>

namespace {

const TCHost* host = nullptr;
tc::TCMod mod;
tc::component_types::Api typesApi{};

/* "CLOK_001" as the little-endian bytes a save carries. */
constexpr uint64_t kClockId = 0x434C4F4B5F303031ULL;
/* The interactive pair ("SWIT_001", "BUTN_001"): one output, no inputs, and a
   one-byte host-owned configuration that holds the level.  The configuration is
   the player's setting and a *host* object, so the UI side may replace it while
   the simulation runs; the callback only reads it. */
constexpr uint64_t kSwitchId = 0x535749545F303031ULL;
constexpr uint64_t kButtonId = 0x4255544E5F303031ULL;

/* The player's level per instance.  It cannot live in the callback's `state`:
   the click happens on the render thread and the callback on the simulation
   thread, so a plain field would be a data race.  One atomic byte per instance
   is well defined in both directions, and the callback only ever reads it. */
struct InteractiveState {
    std::atomic<uint8_t> level{0};
    std::atomic<uint8_t> momentary{0};
};

std::mutex g_stateMutex;
std::map<uint64_t, InteractiveState> g_states;
struct MousePoint { float x, y; };
struct InteractiveHit {
    float min_x, min_y, max_x, max_y;
    bool momentary;
};
std::mutex g_hitMutex;
std::map<uint64_t, InteractiveHit> g_interactiveHits;
void (*g_getMousePos)(MousePoint*) = nullptr;
tc::TCBoardModel g_board;                  /* the game's own selected-component set */
TCBoardApiV4 g_boardApi{};                 /* instance id -> Board sequence index */
tc::component_instances::Api g_instancesApi{};
tc::component_storage::Api g_storageApi{};
TCIoValueApiV1 ioValue{};                  /* the game's own value parser/formatter */
using RefreshSimulationFn = void (*)(void*);
RefreshSimulationFn g_refreshSimulation = nullptr;

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) |
           (uint32_t(a) << 24);
}

struct SwitchConfig {
    uint8_t level;          /* 0 or 1; the UI flips it, the callback reports it */
    uint8_t momentary;      /* 1 for the button: the level only holds while pressed */
};

/* The clock's own configuration: how many cycles the output holds before it
   flips.  1 is the once-per-cycle square wave this Mod has always produced, so
   every instance that never sees a click behaves exactly like the original. */
struct ClockConfig {
    uint8_t period;         /* cycles per level; 1..255, 0 is read as 1 */
    uint8_t reserved;       /* keeps the record the same width as the switch's */
};

constexpr uint32_t kClockConfigSchema = 1;

uint8_t configuredLevel(TCLogicIOV2* io) {
    if (!io || !io->config || io->config_size < sizeof(SwitchConfig)) return 0;
    return static_cast<uint8_t>(io->config[0] & 1u);
}

void note(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

constexpr uint8_t kMinClockPeriod = 1;
constexpr uint8_t kMaxClockPeriod = 255;

/* What the value window accepts.  The loader's io-value service *is* the game's
   own evaluator, so a Mod's value field understands exactly what the game's do;
   without that service - an older loader - a plain literal still works. */
bool parseClockPeriod(const char* text, uint8_t* out) {
    if (out) *out = kMinClockPeriod;
    if (!text) return false;
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text) return false;

    uint64_t value = 0;
    bool parsed = false;
    if (tc::io_value::ready(ioValue)) {
        parsed = tc::io_value::evaluate(ioValue, text, &value) == TC_IO_VALUE_OK;
    }
    if (!parsed) {
        const char* digits = text;
        int base = 10;
        if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            digits += 2;
            base = 16;
        } else if (digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
            digits += 2;
            base = 2;
        }
        char* end = nullptr;
        const unsigned long long literal = std::strtoull(digits, &end, base);
        if (!end || end == digits) return false;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end) return false;
        value = literal;
        parsed = true;
    }
    if (!parsed) return false;
    /* Out of range is clamped rather than refused: the number staying usable
       matters more than the player's exact typo. */
    if (value < kMinClockPeriod) value = kMinClockPeriod;
    if (value > kMaxClockPeriod) value = kMaxClockPeriod;
    if (out) *out = static_cast<uint8_t>(value);
    return true;
}

/* The clickable number box the drawing code paints, one per instance: its
   screen rectangle and the number inside it.  The frame callback hit-tests the
   mouse against these, so what the player clicks is exactly what was painted -
   no second hit-test of its own to drift out of step with the face at some
   other zoom, rotation or window size. */
struct ClockBadge {
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    uint8_t period = kMinClockPeriod;
    uint8_t level = 0;              /* the output the face shows in the middle */
    bool levelKnown = false;        /* false until the callback reports one */
    bool hovered = false;
    bool editing = false;
};

/* The footprint: the rectangle the game hit-tests and reserves for this type.
   It is a *symmetric* box around the component's record point, and a generated
   component's pins sit at +-2 whatever the footprint is (measured: enlarging it
   does not move them), so 2.0 is the widest half-width that still leaves the
   output pin at (2,0) its own lane - the same value the interactive pair next to
   this component uses.  A wider box swallows the pin: the pin's drag area
   becomes the component's, and the reserved space reaches past it.

   Everything painted stays inside this box, so the whole face is clickable,
   draggable and protected from other components being dropped on it. */
constexpr float kFootprintHalfWidth = 2.0f;
constexpr float kFootprintHalfHeight = 1.5f;

/* The face, in board cells.  Its height is the stock constant's own (2.93, see
   docs/research/component-appearance.md: com_constant at 0.048 cells per sprite
   pixel) and its width is everything the footprint above can hold while keeping
   the pin lane: the stock part is 4.92 wide because its pin sits at +3, ours
   sits at +2, so its body ends at 1.55 and starts at -2.0.

     body          3.55 x 2.93    value mark   1.25 across, centred
     number box    1.00 x 0.62    digit height 0.42
     name height   0.34, ending 0.26 from the right edge */
constexpr float kBodyX0 = -kFootprintHalfWidth;
constexpr float kBodyX1 = 1.55f;
constexpr float kBodyY0 = -1.47f;
constexpr float kBodyY1 = 1.46f;

/* The number box in the body's top-left corner: 1.00 x 0.62 cells, 0.20 from
   the left edge and 0.18 from the top, the stock part's own placement. */
constexpr float kBadgeX0 = kBodyX0 + 0.20f;
constexpr float kBadgeY0 = -1.29f;
constexpr float kBadgeX1 = kBadgeX0 + 1.00f;
constexpr float kBadgeY1 = -0.67f;

/* The name row: the same band as the box, ending 0.26 cells short of the body's
   right edge, the way the stock part prints its own six letters there. */
constexpr float kNameX1 = kBodyX1 - 0.26f;
constexpr float kNameX0 = kNameX1 - 1.04f;
constexpr float kNameY0 = -1.24f;
constexpr float kNameY1 = -0.90f;

/* The value mark: centred in the body, a little larger than the stock part's own
   mark so a running clock can be read off the board at a glance (the player
   asked for it to be bigger; it is about the size of the game's own pin ring). */
constexpr float kMarkX = (kBodyX0 + kBodyX1) * 0.5f;
constexpr float kMarkSize = 1.25f;

std::mutex g_badgeMutex;
std::map<uint64_t, ClockBadge> g_badges;

bool badgeOf(uint64_t instance, ClockBadge* out) {
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    const auto found = g_badges.find(instance);
    if (found == g_badges.end()) return false;
    if (out) *out = found->second;
    return true;
}

/* The badge under the pointer.  Badges do not overlap, but the smallest match
   wins anyway so a zoomed-in badge never loses to a zoomed-out neighbour. */
uint64_t badgeAt(float x, float y) {
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    uint64_t hit = 0;
    float best = 1.0e30f;
    for (const auto& entry : g_badges) {
        const ClockBadge& badge = entry.second;
        if (x < badge.min_x || x > badge.max_x || y < badge.min_y || y > badge.max_y)
            continue;
        const float area = (badge.max_x - badge.min_x) * (badge.max_y - badge.min_y);
        if (area < best) {
            best = area;
            hit = entry.first;
        }
    }
    return hit;
}

/* What each live clock is set to.  The configuration bytes stay the durable
   copy: they are what the schematic carries and what the callback obeys.  But
   the render frame's configuration view is looked up by the *board's* component
   id while the binding is keyed by the simulation id - the same two id spaces
   the interactive pair needed atomics for - so the callback mirrors the period
   it reads, one atomic byte per instance, and the face draws the mirror.  That
   keeps the readout right even when the two ids differ, and it moves the moment
   a click lands, whether the board is running or paused. */
struct ClockState {
    std::atomic<uint8_t> period{1};
    std::atomic<uint8_t> level{0};
};

std::mutex g_clockMutex;
std::map<uint64_t, ClockState> g_clocks;

uint8_t clockPeriodOf(TCLogicIOV2* io) {
    uint8_t period = 1;
    if (io->config && io->config_size >= sizeof(ClockConfig)) {
        const uint8_t stored = static_cast<uint8_t>(io->config[0]);
        if (stored >= 1) period = stored;
    }
    std::lock_guard<std::mutex> lock(g_clockMutex);
    auto found = g_clocks.find(io->instance_id);
    if (found == g_clocks.end()) {
        g_clocks[io->instance_id].period.store(period);
        char detail[144] = {};
        snprintf(detail, sizeof(detail),
                 "clock: instance 0x%llx flips every %u cycle(s)",
                 static_cast<unsigned long long>(io->instance_id),
                 static_cast<unsigned>(period));
        note(detail);
        return period;
    }
    found->second.period.store(period);
    return period;
}

/* The output the face shows in the middle, mirrored the same way and for the
   same reason as the period: the callback owns it, the drawing code reads it. */
void noteClockLevel(TCLogicIOV2* io, uint8_t level) {
    std::lock_guard<std::mutex> lock(g_clockMutex);
    g_clocks[io->instance_id].level.store(level & 1u);
}

void switchLogic(TCLogicIOV2* io) {
    if (!io) return;
    /* The player's click owns the value; this callback never changes it (a switch
       that flipped on its own would be exactly the "why does this move by itself"
       the clock caused).  Before the first click the level comes from the
       instance's own configuration so a saved board keeps its setting. */
    InteractiveState* known = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto found = g_states.find(io->instance_id);
        if (found == g_states.end()) {
            InteractiveState& created = g_states[io->instance_id];
            const uint8_t momentary =
                io->config && io->config_size >= sizeof(SwitchConfig)
                    ? static_cast<uint8_t>(io->config[1] & 1u)
                    : 0u;
            created.momentary.store(momentary);
            /* A button can never legitimately be latched high across a reload.
               Clear configuration left by the discarded refresh experiment. */
            created.level.store(momentary ? 0u : configuredLevel(io));
            known = &created;
            /* One line per instance: which id the callback sees, and whether the
               game's selection set answers for it.  The two id spaces are the
               first thing a silent click can be traced to. */
            char detail[160] = {};
            snprintf(detail, sizeof(detail),
                     "interactive: instance 0x%llx level=%u momentary=%u selected=%d",
                     static_cast<unsigned long long>(io->instance_id),
                     static_cast<unsigned>(created.level.load()),
                     static_cast<unsigned>(created.momentary.load()),
                     g_board.valid() ? static_cast<int>(g_board.isComponentSelected(io->instance_id))
                                     : -1);
            note(detail);
        } else {
            known = &found->second;
        }
    }
    if (io->output_count) io->outputs[0] = known->level.load() & 1u;
}

/* The clock source: one output that flips every `period` cycles, with the
   period coming from the instance's own configuration (see ClockConfig).
   `state[0]` is the level, `state[1]` the number of cycles it has held it; both
   are per instance and cleared by RESET, so a board that restarts starts its
   clock at the beginning again. */
void clockLogic(TCLogicIOV2* io) {
    if (!io) return;
    const bool hasState = io->state && io->state_words >= 2;
    if (io->phase == TC_LOGIC_RESET) {
        if (hasState) io->state[0] = 0;
        if (hasState) io->state[1] = 0;
        noteClockLevel(io, 0);
        if (io->output_count) io->outputs[0] = 0;
        return;
    }
    if (io->phase == TC_LOGIC_REFRESH) {
        /* The UI reads this copy: report the current value without advancing.
           The period is refreshed here too, so a click that lands while the
           board is paused is already reflected by the next probe read. */
        (void)clockPeriodOf(io);
        if (hasState) noteClockLevel(io, static_cast<uint8_t>(io->state[0] & 1u));
        if (io->output_count) io->outputs[0] = hasState ? (io->state[0] & 1u) : 0u;
        return;
    }
    const uint8_t period = clockPeriodOf(io);
    if (!hasState) {
        /* No state to count in: keep the original fallback (a constant high)
           instead of pretending to be a clock that cannot remember its phase. */
        noteClockLevel(io, 1);
        if (io->output_count) io->outputs[0] = 1;
        return;
    }
    uint64_t held = io->state[1] + 1u;
    if (held >= period) {
        held = 0;
        io->state[0] ^= 1u;
    }
    io->state[1] = held;
    noteClockLevel(io, static_cast<uint8_t>(io->state[0] & 1u));
    if (io->output_count) io->outputs[0] = io->state[0] & 1u;
}

/* Convert a local, axis-aligned rectangle into the screen-space AABB expected
   by the draw list.  Board rotations are quarter turns, so this preserves the
   intended shape in every component orientation. */
void localBounds(const TCComponentRenderFrameV1& frame,
                 float x0, float y0, float x1, float y1,
                 float* minX, float* minY, float* maxX, float* maxY) {
    float x[4]{}, y[4]{};
    tc::component_render::localToScreen(frame, x0, y0, &x[0], &y[0]);
    tc::component_render::localToScreen(frame, x1, y0, &x[1], &y[1]);
    tc::component_render::localToScreen(frame, x1, y1, &x[2], &y[2]);
    tc::component_render::localToScreen(frame, x0, y1, &x[3], &y[3]);
    *minX = *maxX = x[0];
    *minY = *maxY = y[0];
    for (int i = 1; i < 4; ++i) {
        *minX = std::min(*minX, x[i]);
        *maxX = std::max(*maxX, x[i]);
        *minY = std::min(*minY, y[i]);
        *maxY = std::max(*maxY, y[i]);
    }
}

void localLine(const TCComponentRenderFrameV1& frame,
               float x0, float y0, float x1, float y1,
               uint32_t color, float thickness) {
    float sx0 = 0.f, sy0 = 0.f, sx1 = 0.f, sy1 = 0.f;
    tc::component_render::localToScreen(frame, x0, y0, &sx0, &sy0);
    tc::component_render::localToScreen(frame, x1, y1, &sx1, &sy1);
    frame.draw->line(frame.draw->context, sx0, sy0, sx1, sy1, color, thickness);
}

/* The game's one-bit mark is not a circle and not a rounded square: it is a
   round drop with one square corner (the shape in the reference screenshot).
   A disc plus its local top-right quadrant reproduces that silhouette exactly
   with the primitives exposed by the component-render API. */
void drawTapeBit(const TCComponentRenderFrameV1& frame, uint32_t color,
                 float radius, float centreX, float centreY) {
    const auto* draw = frame.draw;
    const float unit = std::sqrt(frame.axis_x_x * frame.axis_x_x +
                                 frame.axis_x_y * frame.axis_x_y);
    float screenX = 0.f, screenY = 0.f;
    tc::component_render::localToScreen(frame, centreX, centreY, &screenX, &screenY);
    draw->circle_filled(draw->context, screenX, screenY, unit * radius, color);

    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    localBounds(frame, centreX, centreY - radius,
                centreX + radius, centreY,
                &minX, &minY, &maxX, &maxY);
    draw->rect_filled(draw->context, minX, minY, maxX, maxY, color, 0.f);
}

/* ---- the game's own value sprite ------------------------------------------

   The middle of the face shows the clock's level with the art the game itself
   uses for a bit: asset/io_state/io_state.png, 200 x 1100, two columns of drops
   (red for 0, green for 1) on a 100 px grid, eleven shape rows of which the
   first is the saturated drop with its rim.

   It is read from the *game's* asset at load time rather than shipped in the
   package, so the face follows the game's art instead of carrying a copy of it.
   The loader's own texture loader only reads paths inside the package
   (src/native.hpp: it insists on "native/"), so the file is opened from the game
   root, decoded with WIC - the same decoder the loader uses - and uploaded
   through host->create_ui_texture.

   Anything missing on the way (an older loader without the texture API, a build
   whose asset moved, a machine where WIC refuses) leaves g_bitSpriteReady false
   and the hand-drawn drop above is used instead: the readout never disappears. */
constexpr float kSpritePitchPx = 100.f;
constexpr int kSpriteColumnLow = 0;
constexpr int kSpriteColumnHigh = 1;
constexpr int kSpriteRow = 0;

/* Red for 0, green for 1: the sheet's first column is the low state, the second
   the high one. */
constexpr int spriteColumn(uint8_t level) {
    return (level & 1u) ? kSpriteColumnHigh : kSpriteColumnLow;
}

struct SpriteVec2 { float x, y; };
using AddImageQuadFn = void (*)(void*, uint64_t, SpriteVec2, SpriteVec2,
                                SpriteVec2, SpriteVec2, SpriteVec2, SpriteVec2,
                                SpriteVec2, SpriteVec2, uint32_t);

TCUiTexture g_bitSprite{};
bool g_bitSpriteReady = false;
AddImageQuadFn g_addImageQuad = nullptr;
void* (*g_backgroundList)(void*) = nullptr;
void* (*g_mainViewport)() = nullptr;

template <class T>
struct SpriteCom {
    T* p = nullptr;
    ~SpriteCom() { if (p) p->Release(); }
    T** out() { return &p; }
    T* operator->() const { return p; }
};

/* Decode a PNG's bytes to straight-alpha RGBA, exactly the way the loader
   decodes the images it loads for plugins (src/ui_texture.hpp). */
bool decodeSprite(const std::vector<unsigned char>& bytes,
                  std::vector<unsigned char>* rgba, uint32_t* width,
                  uint32_t* height) {
    if (bytes.empty() || bytes.size() > 64u * 1024u * 1024u) return false;
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialised) && initialised != RPC_E_CHANGED_MODE) return false;
    struct Apartment {
        bool owned;
        ~Apartment() { if (owned) CoUninitialize(); }
    } apartment{SUCCEEDED(initialised)};

    SpriteCom<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(factory.out()))))
        return false;
    SpriteCom<IWICStream> stream;
    SpriteCom<IWICBitmapDecoder> decoder;
    SpriteCom<IWICBitmapFrameDecode> frame;
    SpriteCom<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(stream.out())) ||
        FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.p, nullptr,
                                                WICDecodeMetadataCacheOnDemand,
                                                decoder.out())) ||
        FAILED(decoder->GetFrame(0, frame.out())))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 4096 || h > 4096)
        return false;
    if (FAILED(factory->CreateFormatConverter(converter.out())) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4u);
    if (FAILED(converter->CopyPixels(nullptr, w * 4u,
                                     static_cast<UINT>(pixels.size()),
                                     pixels.data())))
        return false;
    *rgba = std::move(pixels);
    *width = w;
    *height = h;
    return true;
}

/* The game root, from this plugin's own data directory (the loader hands out
   <gameRoot>/tc-modloader-data/plugin-data/<mod id>; the same three levels the
   interactive map is read from). */
bool gamePath(const char* relative, std::string* out) {
    if (!host || !host->data_directory_utf8) return false;
    std::string path = host->data_directory_utf8;
    path += "/../../../";
    path += relative;
    *out = path;
    return true;
}

void loadBitSprite(const TCHost* h) {
    if (g_bitSpriteReady) return;
    if (!h->engine_proc) return;
    if (h->size < offsetof(TCHost, release_ui_texture) + sizeof(h->release_ui_texture) ||
        !h->create_ui_texture) {
        note("clock: this loader has no texture API; the value mark stays hand-drawn");
        return;
    }
    void* image = h->engine_proc(h->context, "ImDrawList_AddImageQuad");
    void* background = h->engine_proc(h->context, "igGetBackgroundDrawList");
    void* viewport = h->engine_proc(h->context, "igGetMainViewport");
    if (!image || !background || !viewport) {
        note("clock: the renderer has no image entry points; the value mark stays hand-drawn");
        return;
    }

    std::string path;
    if (!gamePath("asset/io_state/io_state.png", &path)) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        note("clock: asset/io_state/io_state.png not found; the value mark stays hand-drawn");
        return;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    input.close();

    std::vector<unsigned char> rgba;
    uint32_t width = 0, height = 0;
    if (!decodeSprite(bytes, &rgba, &width, &height)) {
        note("clock: the game's bit sprite could not be decoded; the value mark stays hand-drawn");
        return;
    }
    TCUiTexturePixels pixels{};
    pixels.size = sizeof(pixels);
    pixels.width = width;
    pixels.height = height;
    pixels.filter = 0;                     /* linear */
    pixels.rgba = rgba.data();
    pixels.byte_count = rgba.size();
    TCUiTexture created{};
    created.size = sizeof(created);
    if (h->create_ui_texture(h->context, &pixels, &created) != 0) {
        note("clock: the game's bit sprite could not be uploaded; the value mark stays hand-drawn");
        return;
    }
    g_bitSprite = created;
    std::memcpy(&g_addImageQuad, &image, sizeof(g_addImageQuad));
    std::memcpy(&g_backgroundList, &background, sizeof(g_backgroundList));
    std::memcpy(&g_mainViewport, &viewport, sizeof(g_mainViewport));
    g_bitSpriteReady = true;
    char detail[160] = {};
    snprintf(detail, sizeof(detail),
             "clock: the game's own bit sprite is loaded (%ux%u) for the value mark",
             width, height);
    note(detail);
}

/* One cell of that sheet, drawn as a quad whose four corners follow the
   component's own rotation (an axis-aligned image would stand upright inside a
   rotated face). */
void drawSpriteMark(const TCComponentRenderFrameV1& frame, uint8_t level) {
    if (!g_bitSpriteReady || !g_addImageQuad || !g_backgroundList || !g_mainViewport)
        return;
    const int column = spriteColumn(level);
    const float u0 = (column * kSpritePitchPx) / static_cast<float>(g_bitSprite.width);
    const float u1 = ((column + 1) * kSpritePitchPx) / static_cast<float>(g_bitSprite.width);
    const float v0 = (kSpriteRow * kSpritePitchPx) / static_cast<float>(g_bitSprite.height);
    const float v1 = ((kSpriteRow + 1) * kSpritePitchPx) /
                     static_cast<float>(g_bitSprite.height);
    const float half = kMarkSize * 0.5f;
    float corners[4][2] = {};
    const float local[4][2] = {{kMarkX - half, -half},
                               {kMarkX + half, -half},
                               {kMarkX + half, half},
                               {kMarkX - half, half}};
    for (int i = 0; i < 4; ++i)
        tc::component_render::localToScreen(frame, local[i][0], local[i][1],
                                            &corners[i][0], &corners[i][1]);
    SpriteVec2 points[4] = {};
    for (int i = 0; i < 4; ++i) points[i] = SpriteVec2{corners[i][0], corners[i][1]};
    void* list = g_backgroundList(g_mainViewport());
    if (!list) return;
    g_addImageQuad(list, g_bitSprite.renderer_id,
                   points[0], points[1], points[2], points[3],
                   SpriteVec2{u0, v0}, SpriteVec2{u1, v0},
                   SpriteVec2{u1, v1}, SpriteVec2{u0, v1},
                   0xffffffffu);
}

/* The period readout: seven-segment digits drawn from the same vector strokes
   as the wordmark.  Lines instead of the draw API's text primitive keep the
   number crisp at every zoom and rotation (a screen-aligned font string would
   sit at one angle inside a rotated face), and they need no glyph in the game's
   font, which has no reason to carry CJK. */
void drawDigit(const TCComponentRenderFrameV1& frame, int digit,
               float left, float top, float width, float height,
               uint32_t ink, float stroke) {
    /* Bit 0 is the top segment, then clockwise: 1 upper right, 2 lower right,
       3 bottom, 4 lower left, 5 upper left, 6 the middle bar (the usual
       a..g numbering). */
    static const uint8_t segments[10] = {
        0x3f, /* 0 */
        0x06, /* 1 */
        0x5b, /* 2 */
        0x4f, /* 3 */
        0x66, /* 4 */
        0x6d, /* 5 */
        0x7d, /* 6 */
        0x07, /* 7 */
        0x7f, /* 8 */
        0x6f, /* 9 */
    };
    if (digit < 0 || digit > 9) return;
    const uint8_t mask = segments[digit];
    const float right = left + width;
    const float middle = top + height * 0.5f;
    const float bottom = top + height;
    if (mask & 0x01) localLine(frame, left,  top,    right, top,    ink, stroke);
    if (mask & 0x02) localLine(frame, right, top,    right, middle, ink, stroke);
    if (mask & 0x04) localLine(frame, right, middle, right, bottom, ink, stroke);
    if (mask & 0x08) localLine(frame, left,  bottom, right, bottom, ink, stroke);
    if (mask & 0x10) localLine(frame, left,  middle, left,  bottom, ink, stroke);
    if (mask & 0x20) localLine(frame, left,  top,    left,  middle, ink, stroke);
    if (mask & 0x40) localLine(frame, left,  middle, right, middle, ink, stroke);
}

/* Draws `period` centred on (centreX, centreY): one to three digits, the widest
   case (255) still inside the box they are drawn in. */
void drawPeriod(const TCComponentRenderFrameV1& frame, uint32_t period,
                float centreX, float centreY, float width, float height,
                uint32_t ink, float stroke) {
    if (period == 0) period = 1;
    if (period > kMaxClockPeriod) period = kMaxClockPeriod;
    char text[8] = {};
    const int length = snprintf(text, sizeof(text), "%u", static_cast<unsigned>(period));
    if (length <= 0) return;
    const float gap = 0.02f + width * 0.03f;
    const float digits = static_cast<float>(length);
    /* The digits share the given width - three of them are narrower than one
       instead of overflowing the box - and keep a seven-segment digit's own
       proportions instead of stretching to whatever width is left over. */
    const float digitWidth =
        std::min((width - (digits - 1.f) * gap) / digits, height * 0.62f);
    const float digitHeight = std::min(height, digitWidth / 0.62f);
    const float total = digits * digitWidth + (digits - 1.f) * gap;
    float left = centreX - total * 0.5f;
    const float top = centreY - digitHeight * 0.5f;
    for (int i = 0; i < length; ++i) {
        drawDigit(frame, text[i] - '0', left, top, digitWidth, digitHeight, ink, stroke);
        left += digitWidth + gap;
    }
}

/* The number box in the body's top-left corner - the stock constant's own
   arrangement, which is also the game's "click the number to change it" cue.
   It is the only clickable part of the clock: brighter while the pointer is on
   it, amber-rimmed while its value window is open. */
void drawPeriodBadge(const TCComponentRenderFrameV1& frame, uint8_t period,
                     bool hovered, bool editing) {
    const auto* draw = frame.draw;
    const float unit = std::sqrt(frame.axis_x_x * frame.axis_x_x +
                                 frame.axis_x_y * frame.axis_x_y);
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    localBounds(frame, kBadgeX0, kBadgeY0, kBadgeX1, kBadgeY1,
                &minX, &minY, &maxX, &maxY);
    /* The stock box is the body colour taken a step darker, with a thin bright
       rim; hovering lifts it, and an open value window rims it amber. */
    const uint32_t fill = editing ? rgba(58, 74, 34)
                                  : (hovered ? rgba(24, 70, 112) : rgba(20, 52, 82));
    const uint32_t rim = editing ? rgba(240, 200, 96)
                                 : (hovered ? rgba(196, 228, 255) : rgba(150, 196, 235));
    draw->rect_filled(draw->context, minX, minY, maxX, maxY, fill, unit * 0.16f);
    draw->rect(draw->context, minX, minY, maxX, maxY, rim, unit * 0.16f,
               std::max(1.1f, unit * 0.06f));
    drawPeriod(frame, period, kBadgeX0 + (kBadgeX1 - kBadgeX0) * 0.5f,
               kBadgeY0 + (kBadgeY1 - kBadgeY0) * 0.5f,
               (kBadgeX1 - kBadgeX0) - 0.12f, (kBadgeY1 - kBadgeY0) * 0.68f,
               rgba(240, 248, 255), std::max(1.f, unit * 0.07f));
}

/* A Mod-owned clock face laid out like the game's own valued part - the stock
   constant - and at its size: number box in the top-left corner, type name at
   the other end of that row, the value below them.  The value is the live output
   level, drawn with the game's own bit sprite when it could be loaded. */
void renderClock(void*, const TCComponentRenderFrameV1* frame) {
    if (!frame || !frame->draw || frame->custom_id != kClockId) return;
    static bool reported;
    if (!reported) {
        reported = true;
        note("clock: blue CLK face render active");
    }
    const auto* draw = frame->draw;
    const float unit = std::sqrt(frame->axis_x_x * frame->axis_x_x +
                                 frame->axis_x_y * frame->axis_x_y);
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    localBounds(*frame, kBodyX0, kBodyY0, kBodyX1, kBodyY1,
                &minX, &minY, &maxX, &maxY);
    draw->rect_filled(draw->context, minX, minY, maxX, maxY,
                      rgba(35, 96, 151), unit * 0.30f);
    draw->rect(draw->context, minX, minY, maxX, maxY,
               rgba(17, 51, 84), unit * 0.30f, std::max(1.2f, unit * 0.09f));

    /* The name, right-aligned on the box's row the way the stock part prints its
       own six letters. */
    const uint32_t ink = rgba(235, 247, 255);
    const float stroke = std::max(1.3f, unit * 0.085f);
    const float top = kNameY0, bottom = kNameY1, middle = (kNameY0 + kNameY1) * 0.5f;
    const float width = kNameX1 - kNameX0;
    const float gap = width * 0.11f;
    const float letter = (width - 2.f * gap) / 3.f;
    const float c0 = kNameX0, l0 = c0 + letter + gap, k0 = l0 + letter + gap;
    localLine(*frame, c0 + letter, top, c0, top, ink, stroke); /* C */
    localLine(*frame, c0, top, c0, bottom, ink, stroke);
    localLine(*frame, c0, bottom, c0 + letter, bottom, ink, stroke);
    localLine(*frame, l0, top, l0, bottom, ink, stroke); /* L */
    localLine(*frame, l0, bottom, l0 + letter, bottom, ink, stroke);
    localLine(*frame, k0, top, k0, bottom, ink, stroke); /* K */
    localLine(*frame, k0, middle, k0 + letter, top, ink, stroke);
    localLine(*frame, k0, middle, k0 + letter, bottom, ink, stroke);

    /* The number inside the box is the mirror's copy while the simulation has
       run at least once (it moves the instant a value window commits), and the
       configuration the host restored from the save before that. */
    uint8_t period = frame->config && frame->config_size >= sizeof(ClockConfig)
                         ? static_cast<uint8_t>(frame->config[0])
                         : 1u;
    uint8_t level = 0;
    bool levelKnown = false;
    bool hovered = false;
    bool editing = false;
    {
        std::lock_guard<std::mutex> lock(g_clockMutex);
        const auto found = g_clocks.find(frame->instance_id);
        if (found != g_clocks.end()) {
            period = found->second.period.load();
            level = found->second.level.load();
            levelKnown = true;
        }
    }
    if (period < kMinClockPeriod) period = kMinClockPeriod;
    {
        std::lock_guard<std::mutex> lock(g_badgeMutex);
        ClockBadge& badge = g_badges[frame->instance_id];
        float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
        localBounds(*frame, kBadgeX0, kBadgeY0, kBadgeX1, kBadgeY1,
                    &minX, &minY, &maxX, &maxY);
        badge.min_x = minX;
        badge.min_y = minY;
        badge.max_x = maxX;
        badge.max_y = maxY;
        badge.period = period;
        badge.level = level;
        badge.levelKnown = levelKnown;
        hovered = badge.hovered;
        editing = badge.editing;
    }
    drawPeriodBadge(*frame, period, hovered, editing);

    /* The value the part is holding, in the middle: a running clock can be read
       off the board without following its wire.  Nothing is drawn until the
       callback has reported a level, so a board that was never simulated shows
       no invented value. */
    if (levelKnown) {
        if (g_bitSpriteReady) {
            drawSpriteMark(*frame, level);
        } else {
            drawTapeBit(*frame, (level & 1u) ? rgba(34, 177, 78) : rgba(239, 53, 83),
                        kMarkSize * 0.5f, kMarkX, 0.f);
        }
    }
}

/* Both interactive parts own their complete board face.  Their bodies use the
   game's subdued component colours (button blue, latching switch purple), and
   the live value is the original punch-tape bit: red for 0, green for 1. */
void renderInteractive(void*, const TCComponentRenderFrameV1* frame) {
    if (!frame || !frame->draw ||
        (frame->custom_id != kButtonId && frame->custom_id != kSwitchId)) return;
    static bool buttonReported;
    static bool switchReported;
    bool& reported = frame->custom_id == kButtonId ? buttonReported : switchReported;
    if (!reported) {
        reported = true;
        note(frame->custom_id == kButtonId
                 ? "clock: original-style blue button render active"
                 : "clock: original-style purple self-lock switch render active");
    }
    const auto* draw = frame->draw;
    uint8_t level = frame->config && frame->config_size >= sizeof(SwitchConfig)
                        ? static_cast<uint8_t>(frame->config[0] & 1u)
                        : 0u;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        const auto found = g_states.find(frame->instance_id);
        if (found != g_states.end()) level = found->second.level.load() & 1u;
    }
    const float unit = std::sqrt(frame->axis_x_x * frame->axis_x_x +
                                 frame->axis_x_y * frame->axis_x_y);
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    /* The stock output pin is at local (2,0).  Stop the painted body at 1.30
       so the pin dot and the short wire approach remain fully exposed. */
    localBounds(*frame, -1.30f, -1.30f, 1.30f, 1.30f,
                &minX, &minY, &maxX, &maxY);

    const bool isButton = frame->custom_id == kButtonId;
    const uint32_t body = isButton ? rgba(42, 105, 157) : rgba(105, 67, 143);
    const uint32_t rim = isButton ? rgba(25, 67, 103) : rgba(65, 39, 91);
    const uint32_t highlight = isButton ? rgba(72, 142, 193) : rgba(143, 99, 180);
    draw->rect_filled(draw->context, minX, minY, maxX, maxY,
                      body, unit * 0.30f);
    draw->rect(draw->context, minX, minY, maxX, maxY,
               rim, unit * 0.30f, std::max(1.2f, unit * 0.09f));

    /* A short top/left glint gives the same slightly raised, painted-plastic
       treatment as the game's stock coloured controls without adding labels. */
    localBounds(*frame, -1.08f, -1.08f, 1.08f, 1.08f,
                &minX, &minY, &maxX, &maxY);
    draw->rect(draw->context, minX, minY, maxX, maxY,
               highlight, unit * 0.20f, std::max(1.f, unit * 0.05f));

    const uint32_t bit = level ? rgba(34, 177, 78) : rgba(239, 53, 83);
    drawTapeBit(*frame, bit, 0.64f, -0.10f, 0.08f);
}

/* ---- the interactive pair -------------------------------------------------

   The level of a switch/button instance lives in this table, not in the
   callback's `state`: the player's click happens on the render thread while the
   callback runs on the simulation thread, so a plain field would be a data race.
   One atomic byte per instance keeps it well defined in both directions, and the
   callback only ever reads it.

   The click itself needs no hit-testing of our own: the game already point-queries
   its own boards, so a click on the component *selects* it, and the loader hands
   mods the game's selected-component set (tc_board_model.h).  Rule:

     - a left click  -> every instance of ours that is selected gets it
                        (a switch flips its level, a button goes high)
     - left release  -> every pressed button goes back low

   Clicking anywhere else clears the selection first, so nothing toggles; dragging
   the component is a press *plus* movement of the same selection and therefore
   toggles at most once, on the press. */
/* These are cimgui's overload-qualified export names.  They must be resolved
   through TCHost::engine_proc: the process module is the loader proxy, while
   engine_proc deliberately returns the corresponding real-engine address. */
bool (*g_mouseClicked)(int, bool) = nullptr;
bool (*g_mouseReleased)(int) = nullptr;
bool g_clickArmed = true;
bool g_pendingMouseClick = false;
int32_t g_pendingMouseFrame = -1;
std::atomic<uint64_t> g_pressed{0};        /* the button instance held down */
std::atomic<uint64_t> g_toggles{0};        /* for the playtest to read back */
uint64_t g_lastSelectionCount = ~0ull;     /* diagnostic: log selection changes */
std::map<uint64_t, uint64_t> g_parentOf;   /* instance id -> top-level id */
std::map<uint64_t, uint64_t> g_selectionKeyOf; /* instance id -> selection-set key */
std::vector<TCGameHandle> g_componentHandles;
uint64_t g_mapAge = 0;                     /* frames since the map was refreshed */

/* The loader publishes "instance top-level" pairs next to the game on every
   compile (interactive-map.txt); the Mod's own data directory sits inside
   <gameRoot>/tc-modloader-data/plugin-data/<mod>, so the file is three levels
   up.  Without it the selection set cannot be matched to our instances. */
void refreshParentMap() {
    if (!host || !host->data_directory_utf8) return;
    std::string path = host->data_directory_utf8;
    path += "/../../../interactive-map.txt";
    std::ifstream in(path);
    if (!in) return;
    std::map<uint64_t, uint64_t> fresh;
    std::string instance, parent;
    while (in >> instance >> parent) {
        fresh[std::strtoull(instance.c_str(), nullptr, 16)] =
            std::strtoull(parent.c_str(), nullptr, 16);
    }
    if (!fresh.empty()) g_parentOf.swap(fresh);
}

uint64_t topLevelOf(uint64_t instance) {
    auto found = g_parentOf.find(instance);
    return found == g_parentOf.end() ? instance : found->second;
}

/* get_component_id() returns a component's sequence index, and that index is
   what the game's selected-components hash set stores.  The logic callback,
   on the other hand, is addressed by the component's persistent 64-bit id.
   Build the bridge from the supported Board snapshot service instead of
   treating those two id spaces as interchangeable. */
bool refreshSelectionKeysDirect() {
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
        return false;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &board, &raw) != TC_HANDLE_OK || !raw) return false;
    const auto* bytes = static_cast<const unsigned char*>(raw);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, bytes + 0x78, sizeof(count));
    std::memcpy(&data, bytes + 0x80, sizeof(data));
    if (!data || count > 1000000ull) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(data, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_NOACCESS))
        return false;
    std::map<uint64_t, uint64_t> fresh;
    const unsigned char* first = data + 8;
    for (uint64_t index = 0; index < count; ++index) {
        const unsigned char* record = first + index * 0x238ull;
        if (record[0] != 0x4e) continue;
        uint64_t prototype = 0, instance = 0;
        std::memcpy(&prototype, record + 0x188, sizeof(prototype));
        if (prototype != kSwitchId && prototype != kButtonId && prototype != kClockId)
            continue;
        std::memcpy(&instance, record + 0x08, sizeof(instance));
        fresh[instance] = index;
    }
    g_selectionKeyOf.swap(fresh);
    return true;
}

void refreshSelectionKeys() {
    if (!g_boardApi.get_current || !g_boardApi.capture_objects ||
        !g_boardApi.read_component) {
        (void)refreshSelectionKeysDirect();
        return;
    }
    TCGameHandle board{};
    if (g_boardApi.get_current(g_boardApi.context, &board) != TC_HANDLE_OK) {
        (void)refreshSelectionKeysDirect();
        return;
    }
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    int status = g_boardApi.capture_objects(g_boardApi.context, &board, &snapshot,
                                             sizeof(snapshot), &buffers);
    if (status != TC_SNAPSHOT_ERR_CAPACITY && status != TC_SNAPSHOT_OK) {
        (void)refreshSelectionKeysDirect();
        return;
    }
    g_componentHandles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
    buffers.components = g_componentHandles.data();
    buffers.component_capacity = g_componentHandles.size();
    status = g_boardApi.capture_objects(g_boardApi.context, &board, &snapshot,
                                        sizeof(snapshot), &buffers);
    if (status != TC_SNAPSHOT_OK) {
        (void)refreshSelectionKeysDirect();
        return;
    }
    std::map<uint64_t, uint64_t> fresh;
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (g_boardApi.read_component(g_boardApi.context, &g_componentHandles[index], &info,
                                      sizeof(info)) != TC_SNAPSHOT_OK)
            continue;
        if (!(info.flags & TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE)) continue;
        if (info.custom_prototype_id != kSwitchId && info.custom_prototype_id != kButtonId &&
            info.custom_prototype_id != kClockId)
            continue;
        fresh[info.id] = index;
    }
    g_selectionKeyOf.swap(fresh);
}

uint64_t selectionKeyOf(uint64_t instance) {
    const uint64_t top = topLevelOf(instance);
    auto found = g_selectionKeyOf.find(top);
    if (found == g_selectionKeyOf.end()) found = g_selectionKeyOf.find(instance);
    return found == g_selectionKeyOf.end() ? ~uint64_t(0) : found->second;
}

bool instanceSelected(uint64_t instance, bool previous = false) {
    const uint64_t key = selectionKeyOf(instance);
    if (key != ~uint64_t(0))
        return previous ? g_board.isComponentPreviouslySelected(key)
                        : g_board.isComponentSelected(key);
    /* Retain the id-shaped check for loader/game builds whose selection set is
       keyed directly by component id. */
    const uint64_t top = topLevelOf(instance);
    return previous ? g_board.isComponentPreviouslySelected(top)
                    : g_board.isComponentSelected(top);
}

/* Publish a UI level immediately, including while the simulator is paused.
   `reset` is deliberately per-instance: it invokes this component's callback
   and commits its outputs without stepping or resetting the rest of the board.
   These interactive definitions have no simulation-state words, so there is no
   state to lose. */
bool publishInteractiveLevel(uint64_t instance, bool momentary) {
    if (!g_instancesApi.enumerate || !g_instancesApi.reset) return false;
    const uint64_t customId = momentary ? kButtonId : kSwitchId;
    std::vector<TCComponentInstanceHandle> handles(16);
    uint32_t written = 0, total = 0;
    int status = tc::component_instances::enumerate(
        g_instancesApi, customId, handles.data(),
        static_cast<uint32_t>(handles.size()), &written, &total);
    if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
        handles.resize(total);
        status = tc::component_instances::enumerate(
            g_instancesApi, customId, handles.data(),
            static_cast<uint32_t>(handles.size()), &written, &total);
    }
    if (status != TC_COMPONENT_INSTANCES_OK) return false;
    for (uint32_t i = 0; i < written; ++i) {
        if (handles[i].instance_id != instance) continue;
        const int resetStatus = tc::component_instances::reset(g_instancesApi, handles[i]);
        if (resetStatus == TC_COMPONENT_INSTANCES_OK) return true;
        char detail[160] = {};
        snprintf(detail, sizeof(detail),
                 "interactive: immediate output publish failed for 0x%llx: %s",
                 static_cast<unsigned long long>(instance),
                 tc::component_instances::errorText(resetStatus));
        note(detail);
        return false;
    }
    return false;
}

/* Store the level in the host-owned instance configuration before asking the
   game to refresh.  A refresh may produce a fresh binding/instance id; the new
   callback then starts from these bytes instead of falling back to level 0. */
bool storeInteractiveLevel(uint64_t instance, bool momentary, uint8_t level) {
    if (!g_instancesApi.enumerate || !g_storageApi.write_config) return false;
    const uint64_t customId = momentary ? kButtonId : kSwitchId;
    std::vector<TCComponentInstanceHandle> handles(16);
    uint32_t written = 0, total = 0;
    int status = tc::component_instances::enumerate(
        g_instancesApi, customId, handles.data(),
        static_cast<uint32_t>(handles.size()), &written, &total);
    if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
        handles.resize(total);
        status = tc::component_instances::enumerate(
            g_instancesApi, customId, handles.data(),
            static_cast<uint32_t>(handles.size()), &written, &total);
    }
    if (status != TC_COMPONENT_INSTANCES_OK) return false;
    for (uint32_t i = 0; i < written; ++i) {
        if (handles[i].instance_id != instance) continue;
        const SwitchConfig config{static_cast<uint8_t>(level & 1u),
                                  static_cast<uint8_t>(momentary ? 1u : 0u)};
        return tc::component_storage::writeConfig(
                   g_storageApi, handles[i], 1u, &config, sizeof(config)) ==
               TC_COMPONENT_STORAGE_OK;
    }
    return false;
}

/* Commit the clock's period to the instance's own configuration.  The record is
   the durable copy - it is what the schematic carries and what the next load
   binds from - and the same call updates the host's live copy, which is what
   the callback reads on the cycles that follow. */
bool storeClockPeriod(uint64_t instance, uint8_t period) {
    if (!g_instancesApi.enumerate || !g_storageApi.write_config) return false;
    std::vector<TCComponentInstanceHandle> handles(16);
    uint32_t written = 0, total = 0;
    int status = tc::component_instances::enumerate(
        g_instancesApi, kClockId, handles.data(),
        static_cast<uint32_t>(handles.size()), &written, &total);
    if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
        handles.resize(total);
        status = tc::component_instances::enumerate(
            g_instancesApi, kClockId, handles.data(),
            static_cast<uint32_t>(handles.size()), &written, &total);
    }
    if (status != TC_COMPONENT_INSTANCES_OK) return false;
    for (uint32_t i = 0; i < written; ++i) {
        if (handles[i].instance_id != instance) continue;
        const ClockConfig config{period, 0};
        return tc::component_storage::writeConfig(g_storageApi, handles[i],
                                                  kClockConfigSchema, &config,
                                                  sizeof(config)) ==
               TC_COMPONENT_STORAGE_OK;
    }
    return false;
}

/* The binding output above is what the generated program reads next.  While the
   game is paused, however, its wire-value buffer is intentionally frozen.  The
   stock constant editor solves the same problem with sim_stop_and_refresh:
   recompute the current board in place without advancing the cycle. */
bool refreshPausedBoard() {
    if (!g_refreshSimulation) return false;
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
        return false;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &board, &raw) != TC_HANDLE_OK || !raw) return false;
    g_refreshSimulation(const_cast<void*>(raw));
    return true;
}

void interactiveFrameExperimental(void* user, const TCFrame* frame) {
    (void)user;
    (void)frame;
    if (!g_board.valid()) return;
    if ((g_mapAge++ % 30u) == 0u) refreshParentMap();
    /* Object handles are frame-scoped, so enumerate in the same frame they are
       consumed.  Boards are small and this only retains the two interactive
       component types. */
    refreshSelectionKeys();
    /* Two ways in, because this build does not export the ImGui mouse queries by
       name (measured: `igIsMouseClicked`/`igIsMouseReleased` resolve to null):

       - with the queries, a click is a click and a button is momentary;
       - without them the game's own selection is the signal: the game clears and
         re-adds the selection when a component is clicked, so "selected now, not
         selected in the previous frame" is the click.  A button then pulses for
         exactly one frame, which the front end turns into one simulated cycle. */
    const bool haveMouse = g_mouseClicked != nullptr && g_mouseReleased != nullptr;
    bool released = haveMouse && g_mouseReleased && g_mouseReleased(0);
    const bool rawClicked = haveMouse && g_mouseClicked(0, false);
    /* The host can dispatch several Mod callbacks for one physical press, and
       this engine can expose the same ImGui click edge across those callbacks.
       Do not infer re-arming from one false sample between them: only an actual
       release permits the next click. */
    bool clicked = rawClicked && g_clickArmed;
    if (clicked) g_clickArmed = false;
    if (released && !rawClicked) g_clickArmed = true;
    uint64_t directTarget = 0;
    if (clicked && g_getMousePos) {
        MousePoint mouse{-100000.f, -100000.f};
        g_getMousePos(&mouse);
        float bestArea = 1.0e30f;
        std::lock_guard<std::mutex> lock(g_hitMutex);
        for (const auto& hit : g_interactiveHits) {
            const auto& box = hit.second;
            if (mouse.x < box.min_x || mouse.x > box.max_x ||
                mouse.y < box.min_y || mouse.y > box.max_y)
                continue;
            const float area = (box.max_x - box.min_x) * (box.max_y - box.min_y);
            if (area < bestArea) {
                bestArea = area;
                directTarget = hit.first;
            }
        }
    }
    if (!haveMouse) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (auto& entry : g_states) {
            const bool now = instanceSelected(entry.first);
            const bool before = instanceSelected(entry.first, true);
            if (now && !before) {
                clicked = true;
                char detail[96] = {};
                snprintf(detail, sizeof(detail),
                         "interactive: click detected on 0x%llx (selected=%d prev=%d)",
                         static_cast<unsigned long long>(entry.first),
                         static_cast<int>(now), static_cast<int>(before));
                note(detail);
            }
        }
    }
    /* Diagnostic: whether the game's selection ever contains one of ours.  A
       click that toggles nothing is otherwise indistinguishable from a click we
       never saw. */
    const uint64_t count = g_board.selectedComponentCount();
    if (count != g_lastSelectionCount) {
        g_lastSelectionCount = count;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (const auto& entry : g_states) {
            char detail[128] = {};
            snprintf(detail, sizeof(detail),
                     "interactive: selection=%llu actual0=%llu ours 0x%llx key=%llu selected=%d (prev=%d) mouse=%d",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(count ? g_board.selectedComponentIdAt(0) : ~uint64_t(0)),
                     static_cast<unsigned long long>(entry.first),
                     static_cast<unsigned long long>(selectionKeyOf(entry.first)),
                     static_cast<int>(instanceSelected(entry.first)),
                     static_cast<int>(instanceSelected(entry.first, true)),
                     g_mouseClicked ? 1 : 0);
            note(detail);
        }
    }
    if (clicked) {
        std::vector<std::pair<uint64_t, bool>> changed;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            for (auto& entry : g_states) {
                /* The simulation id may be a child of the board component id.  The
                   selection model always contains the top-level id, so use the same
                   mapping here that the diagnostics and fallback path use. */
                if (g_getMousePos) {
                    if (!directTarget || entry.first != directTarget) continue;
                } else if (!instanceSelected(entry.first)) {
                    continue;
                }
                InteractiveState& state = entry.second;
                const bool momentary = state.momentary.load() != 0;
                if (momentary) {
                    state.level.store(1);
                    g_pressed.store(entry.first);
                } else {
                    state.level.store(state.level.load() ? 0 : 1);
                }
                changed.emplace_back(entry.first, momentary);
                g_toggles.fetch_add(1);
                char detail[128] = {};
                snprintf(detail, sizeof(detail),
                         "interactive: applied 0x%llx key=%llu level=%u momentary=%u",
                         static_cast<unsigned long long>(entry.first),
                         static_cast<unsigned long long>(selectionKeyOf(entry.first)),
                         static_cast<unsigned>(state.level.load()),
                         static_cast<unsigned>(state.momentary.load()));
                note(detail);
            }
        }
        for (const auto& change : changed) {
            uint8_t level = 0;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto found = g_states.find(change.first);
                if (found != g_states.end()) level = found->second.level.load() & 1u;
            }
            note(storeInteractiveLevel(change.first, change.second, level)
                     ? "interactive: level stored before board refresh"
                     : "interactive: level storage failed");
            if (publishInteractiveLevel(change.first, change.second)) {
                char detail[112] = {};
                snprintf(detail, sizeof(detail),
                         "interactive: output published immediately for 0x%llx",
                         static_cast<unsigned long long>(change.first));
                note(detail);
            }
            note(refreshPausedBoard()
                     ? (change.second
                            ? "interactive: board refreshed for button press"
                            : "interactive: board refreshed for self-lock switch")
                     : "interactive: board refresh unavailable");
        }
    }
    if (released) {
        if (g_pressed.exchange(0)) {
            std::vector<uint64_t> releasedButtons;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                for (auto& entry : g_states) {
                    if (!entry.second.momentary.load() || !entry.second.level.load()) continue;
                    entry.second.level.store(0);
                    releasedButtons.push_back(entry.first);
                }
            }
            for (const uint64_t id : releasedButtons) {
                (void)storeInteractiveLevel(id, true, 0);
                (void)publishInteractiveLevel(id, true);
            }
            (void)refreshPausedBoard();
            note("interactive: button released and board refreshed");
        }
    } else if (!haveMouse) {
        /* The fallback has no release event: a momentary component is a one-frame
           pulse instead, and it is armed here on the frame after the click. */
        if (g_pressed.exchange(0)) {
            std::vector<uint64_t> releasedButtons;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                for (auto& entry : g_states) {
                    if (!entry.second.momentary.load() || !entry.second.level.load()) continue;
                    entry.second.level.store(0);
                    releasedButtons.push_back(entry.first);
                }
            }
            for (const uint64_t id : releasedButtons) {
                (void)storeInteractiveLevel(id, true, 0);
                (void)publishInteractiveLevel(id, true);
            }
            (void)refreshPausedBoard();
        }
    }
}

/* ---- the value window ------------------------------------------------------

   The number in the corner box is the player's setting, and clicking the box
   opens a small window beside it where the number can be typed - the same
   "click the value, type a value" affordance the game's own parts have.  It is
   the game's own ImGui (through tc_ui), so it types and looks like the game's
   value fields, and the text goes through the game's own evaluator (tc.io_value)
   before it lands in the instance's configuration. */

uint64_t g_editingClock = 0;   /* the board component the window belongs to */
char g_editorText[16] = {};
bool g_focusEditor = false;
bool g_editorHovered = false;  /* the pointer was on the window last frame */
std::string g_editorMessage;

/* Which instance the window writes to.  The clicked box names the board's own
   component id; the binding - and so the configuration write - is keyed by the
   simulation id, which is that same id unless the clock came out of a flattened
   custom component. */
uint64_t editorTargetInstance() {
    std::lock_guard<std::mutex> lock(g_clockMutex);
    for (const auto& entry : g_clocks)
        if (topLevelOf(entry.first) == g_editingClock) return entry.first;
    return g_editingClock;
}

void closeClockEditor(const char* why) {
    if (!g_editingClock) return;
    const uint64_t closed = g_editingClock;
    g_editingClock = 0;
    g_editorMessage.clear();
    {
        std::lock_guard<std::mutex> lock(g_badgeMutex);
        const auto found = g_badges.find(closed);
        if (found != g_badges.end()) found->second.editing = false;
    }
    g_editorHovered = false;
    note(std::string("clock: value window closed: ") + why);
}

void openClockEditor(uint64_t instance, uint8_t period) {
    if (g_editingClock && g_editingClock != instance) closeClockEditor("another clock was clicked");
    g_editingClock = instance;
    g_editorMessage.clear();
    snprintf(g_editorText, sizeof(g_editorText), "%u", static_cast<unsigned>(period));
    /* The player clicked the number to change it: hand the keyboard straight to
       the field instead of making them click a second time. */
    g_focusEditor = true;
    {
        std::lock_guard<std::mutex> lock(g_badgeMutex);
        const auto found = g_badges.find(instance);
        if (found != g_badges.end()) found->second.editing = true;
    }
    char detail[160] = {};
    snprintf(detail, sizeof(detail),
             "clock: value window opened for 0x%llx (period %u)",
             static_cast<unsigned long long>(instance), static_cast<unsigned>(period));
    note(detail);
}

void commitClockEditor() {
    uint8_t period = kMinClockPeriod;
    if (!parseClockPeriod(g_editorText, &period)) {
        g_editorMessage = "看不懂这个数字：可以写 8、0x10 或 1+7。";
        note(std::string("clock: value window refused \"") + g_editorText + "\"");
        return;
    }
    const uint64_t target = editorTargetInstance();
    if (!storeClockPeriod(target, period)) {
        g_editorMessage = "写入元件配置失败：这个元件可能已经不在棋盘上了。";
        note("clock: value window could not store the period");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_clockMutex);
        const auto found = g_clocks.find(target);
        if (found != g_clocks.end()) found->second.period.store(period);
    }
    char detail[160] = {};
    snprintf(detail, sizeof(detail), "clock: 0x%llx now flips every %u cycle(s)",
             static_cast<unsigned long long>(target), static_cast<unsigned>(period));
    note(detail);
    closeClockEditor("value accepted");
}

/* Drawn every frame while a window is open, next to the box that opened it, so
   panning or zooming the board carries the window along with its component. */
void drawClockEditor() {
    if (!g_editingClock) return;
    ClockBadge badge{};
    const bool placed = badgeOf(g_editingClock, &badge);
    constexpr float kEditorWidth = 340.f;
    /* Beside the box it belongs to, on the side with room for it: a clock near
       the right edge would otherwise push the window off screen. */
    tc::ui::Vec2 position{-1.f, -1.f};
    if (placed) {
        const tc::ui::Vec2 viewport = tc::ui::viewportSize();
        const bool rightFits = viewport.x <= 0.f ||
                               badge.max_x + 14.f + kEditorWidth <= viewport.x;
        position = rightFits
                       ? tc::ui::Vec2{badge.max_x + 14.f, badge.min_y - 6.f}
                       : tc::ui::Vec2{badge.min_x - 14.f - kEditorWidth, badge.min_y - 6.f};
    }
    if (auto panel = tc::ui::panel("时钟周期###TCLocalClockPeriod", nullptr,
                                   {kEditorWidth, 0.f}, position, tc::ui::kPanelFontScale,
                                   tc::ui::kToolbarFlags,
                                   placed ? tc::ui::Cond_Always : tc::ui::Cond_Once)) {
        /* Remembered for the next frame's click handling: a click that lands on
           this window must not also be read as a click on a box behind it. */
        g_editorHovered = tc::ui::isWindowHovered(0);
        /* One line per opened window, plus one per change of the field: enough
           to tell "the window never drew" from "the keys never arrived" in a
           log-only playtest. */
        static uint64_t reported = 0;
        if (reported != g_editingClock) {
            reported = g_editingClock;
            char detail[160] = {};
            snprintf(detail, sizeof(detail), "clock: value window drew for 0x%llx",
                     static_cast<unsigned long long>(g_editingClock));
            note(detail);
        }
        static std::string lastText;
        if (lastText != g_editorText) {
            lastText = g_editorText;
            note("clock: value window text \"" + lastText + "\"");
        }
        tc::ui::text("时钟源：每 N 个周期翻转一次（N = 1 就是原版每周期翻转）");
        tc::ui::textDisabled("与游戏的数值框同语法，可以写表达式：8、0x10、1+7");
        if (g_focusEditor) {
            /* Focus lands on the field in this same pass, so typing can start
               right after the click that opened the window. */
            tc::ui::setKeyboardFocusHere(0);
            g_focusEditor = false;
        }
        tc::ui::setNextItemWidth(110.f);
        /* No input-text flags: the field's return value says "the text changed",
           not "the player pressed Enter" (measured - a Backspace commits too),
           so the value is committed the way the game's own fields do it, when
           the edit ends: Enter, clicking away, or the button below. */
        tc::ui::inputText("##period", g_editorText, sizeof(g_editorText));
        const bool editEnded = tc::ui::isItemDeactivatedAfterEdit();
        {
            static bool focusReported = false;
            if (!focusReported && tc::ui::isItemFocused()) {
                focusReported = true;
                note("clock: value window field has the keyboard");
            }
        }
        tc::ui::sameLine();
        const bool accept = tc::ui::button("确定");
        tc::ui::sameLine();
        const bool cancel = tc::ui::button("取消");
        uint8_t parsed = kMinClockPeriod;
        if (parseClockPeriod(g_editorText, &parsed)) {
            char line[128] = {};
            snprintf(line, sizeof(line), "= 每 %u 个周期翻转一次，完整方波 %u 个周期",
                     static_cast<unsigned>(parsed),
                     2u * static_cast<unsigned>(parsed));
            tc::ui::text(line);
        } else if (g_editorText[0]) {
            tc::ui::textDisabled("这个表达式看不懂");
        }
        if (!g_editorMessage.empty()) tc::ui::textDisabled(g_editorMessage.c_str());
        if (editEnded || accept) {
            note(accept ? "clock: value window commit (button)"
                        : "clock: value window commit (the edit ended)");
            commitClockEditor();
        }
        else if (cancel || tc::ui::keys::pressed(tc::ui::Key_Escape))
            closeClockEditor("cancelled");
    }
}

/* The driver build (build.ps1 -DTC_CLOCK_DRIVER, package dev.clock-driver) is
   the same plugin plus an in-process driver that clicks the box and types into
   the window it opens; see tests/clock-period-playtest.ps1. */
#ifdef TC_CLOCK_DRIVER
#include "../../tests/clock-period-driver.hpp"
#endif

/* Proven interaction path restored from the 17:29 direct-map build.  This is
   intentionally kept independent of drawing: visual ownership must not alter
   how the simulator receives a switch/button level. */
void interactiveFrame(void* user, const TCFrame* frame) {
    (void)user;
    if (!g_board.valid()) {
        /* Off the board there is nothing to hit-test and no window to keep. */
        if (g_editingClock) closeClockEditor("the board went away");
        return;
    }
    if ((g_mapAge++ % 30u) == 0u) refreshParentMap();
    refreshSelectionKeys();

    const bool haveMouse = g_mouseClicked != nullptr;
    bool clicked = false;
    /* Where the pointer is, once per frame: it drives the box's hover highlight
       and, on the click below, which box was hit.  Both are read here rather
       than in the drawing code so all pointer state comes from one place. */
    MousePoint mouse{-100000.f, -100000.f};
    if (g_getMousePos) g_getMousePos(&mouse);
    {
        std::lock_guard<std::mutex> lock(g_badgeMutex);
        for (auto& entry : g_badges) {
            const ClockBadge& badge = entry.second;
            entry.second.hovered = mouse.x >= badge.min_x && mouse.x <= badge.max_x &&
                                   mouse.y >= badge.min_y && mouse.y <= badge.max_y;
        }
    }
    if (haveMouse && g_mouseClicked(0, false)) {
        g_pendingMouseClick = true;
        g_pendingMouseFrame = frame ? frame->frame_number : -1;
    }
    /* Board selection is updated after the mouse edge.  Wait two complete
       engine frames before consuming it: clicking blank space then sees an
       empty selection instead of toggling the component that was selected
       before the click.  Re-clicking an already selected control still works. */
    if (haveMouse && g_pendingMouseClick &&
        (!frame || g_pendingMouseFrame < 0 ||
         frame->frame_number >= g_pendingMouseFrame + 2)) {
        clicked = true;
        g_pendingMouseClick = false;
    }
    const bool released = haveMouse && g_mouseReleased && g_mouseReleased(0);
    if (!haveMouse) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (auto& entry : g_states) {
            const bool now = instanceSelected(entry.first);
            const bool before = instanceSelected(entry.first, true);
            if (now && !before) {
                clicked = true;
                char detail[96] = {};
                snprintf(detail, sizeof(detail),
                         "interactive: click detected on 0x%llx (selected=%d prev=%d)",
                         static_cast<unsigned long long>(entry.first),
                         static_cast<int>(now), static_cast<int>(before));
                note(detail);
            }
        }
    }

    const uint64_t count = g_board.selectedComponentCount();
    if (count != g_lastSelectionCount) {
        g_lastSelectionCount = count;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (const auto& entry : g_states) {
            char detail[128] = {};
            snprintf(detail, sizeof(detail),
                     "interactive: selection=%llu actual0=%llu ours 0x%llx key=%llu selected=%d (prev=%d) mouse=%d",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(count ? g_board.selectedComponentIdAt(0)
                                                           : ~uint64_t(0)),
                     static_cast<unsigned long long>(entry.first),
                     static_cast<unsigned long long>(selectionKeyOf(entry.first)),
                     static_cast<int>(instanceSelected(entry.first)),
                     static_cast<int>(instanceSelected(entry.first, true)),
                     g_mouseClicked ? 1 : 0);
            note(detail);
        }
    }

    if (clicked) {
        /* A click on a clock's number box opens its value window.  The box is
           the only clickable part of the clock, so a click that selects or
           drags the component itself changes nothing - the setting only moves
           when the player types a number. */
        const uint64_t box = (!g_editorHovered && g_getMousePos)
                                 ? badgeAt(mouse.x, mouse.y)
                                 : 0;
        {
            uint64_t first = 0;
            ClockBadge shown{};
            {
                std::lock_guard<std::mutex> lock(g_badgeMutex);
                const auto it = g_badges.begin();
                if (it != g_badges.end()) {
                    first = it->first;
                    shown = it->second;
                }
            }
            char detail[192] = {};
            snprintf(detail, sizeof(detail),
                     "clock: click at (%.0f,%.0f); box 0x%llx is (%.0f,%.0f)-(%.0f,%.0f), hit=%llu",
                     mouse.x, mouse.y, static_cast<unsigned long long>(first),
                     shown.min_x, shown.min_y, shown.max_x, shown.max_y,
                     static_cast<unsigned long long>(box));
            note(detail);
        }
        if (box) {
            ClockBadge badge{};
            if (badgeOf(box, &badge)) openClockEditor(box, badge.period);
        }
        std::vector<std::pair<uint64_t, bool>> changed;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            for (auto& entry : g_states) {
                if (!instanceSelected(entry.first)) continue;
                InteractiveState& state = entry.second;
                const bool momentary = state.momentary.load() != 0;
                if (momentary) {
                    state.level.store(1);
                    g_pressed.store(entry.first);
                } else {
                    state.level.store(state.level.load() ? 0 : 1);
                }
                changed.emplace_back(entry.first, momentary);
                g_toggles.fetch_add(1);
                char detail[128] = {};
                snprintf(detail, sizeof(detail),
                         "interactive: applied 0x%llx key=%llu level=%u momentary=%u",
                         static_cast<unsigned long long>(entry.first),
                         static_cast<unsigned long long>(selectionKeyOf(entry.first)),
                         static_cast<unsigned>(state.level.load()),
                         static_cast<unsigned>(state.momentary.load()));
                note(detail);
            }
        }
        /* A cyclic board has no game-compiled program to publish this source.
           Commit the host-owned configuration and run only this instance's
           callback; the standalone simulator reads the resulting wire byte. */
        for (const auto& change : changed) {
            uint8_t level = 0;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto found = g_states.find(change.first);
                if (found != g_states.end()) level = found->second.level.load() & 1u;
            }
            (void)storeInteractiveLevel(change.first, change.second, level);
            (void)publishInteractiveLevel(change.first, change.second);
        }
    }
    if (released) {
        const uint64_t held = g_pressed.exchange(0);
        if (held) {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto found = g_states.find(held);
                if (found != g_states.end()) found->second.level.store(0);
            }
            (void)storeInteractiveLevel(held, true, 0);
            (void)publishInteractiveLevel(held, true);
        }
    } else if (!haveMouse) {
        const uint64_t held = g_pressed.exchange(0);
        if (held) {
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                const auto found = g_states.find(held);
                if (found != g_states.end()) found->second.level.store(0);
            }
            (void)storeInteractiveLevel(held, true, 0);
            (void)publishInteractiveLevel(held, true);
        }
    }

    /* The value window last, so it draws above the board and the edits it
       reports are applied in the same frame the player made them. */
    if (g_editingClock && tc::ui::ready()) drawClockEditor();

#ifdef TC_CLOCK_DRIVER
    tc_clock_driver::driver().tick(frame);
#endif
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || !out) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    /* The interactive pair needs the game's own selection set and two mouse
       queries; both are soft (the components still register and hold their saved
       setting if either is missing). */
    g_board.load(h);
    (void)tc::boardService(h, &g_boardApi);
    (void)tc::component_instances::table(h, &g_instancesApi);
    (void)tc::component_storage::table(h, &g_storageApi);
    /* Soft dependencies for the value window: the game's own ImGui (through
       tc_ui) and its own expression evaluator (tc.io_value).  Without either,
       the corner box still shows the period - only editing it is missing. */
    const bool uiReady = tc::ui::load(h);
    (void)tc::io_value::table(h, &ioValue);
    /* The value mark's art: the game's own asset/io_state/io_state.png, decoded
       and uploaded here so the middle of the face is the game's sprite and not a
       hand-drawn lookalike (with a fallback when any step is unavailable). */
    loadBitSprite(h);
    if (h->log) {
        h->log(h->context,
               (std::string("clock: value window ") +
                (uiReady ? "armed" : ("without UI (" + tc::ui::missing() + ")")) +
                (tc::io_value::ready(ioValue) ? "; the game's own value parser is available"
                                              : "; falling back to literal numbers"))
                   .c_str());
    }
    /* cimgui exports overloads with a suffix in this engine build.  The old
       unsuffixed GetProcAddress lookup always returned null, which left both
       components on the selection-only fallback; that fallback cannot observe
       a click on an already-selected component and made the controls appear
       completely inert. */
    if (h->engine_proc) {
        g_mouseClicked = reinterpret_cast<bool (*)(int, bool)>(
            h->engine_proc(h->context, "igIsMouseClicked_Bool"));
        g_mouseReleased = reinterpret_cast<bool (*)(int)>(
            h->engine_proc(h->context, "igIsMouseReleased_Nil"));
        void* mousePos = h->engine_proc(h->context, "igGetMousePos");
        std::memcpy(&g_getMousePos, &mousePos, sizeof(g_getMousePos));
    }
    if (h->resolve_alias) {
        g_refreshSimulation = reinterpret_cast<RefreshSimulationFn>(
            h->resolve_alias(h->context, "io.constant.refresh"));
    }
    if (out->size >= offsetof(TCPlugin, on_unload) + sizeof(out->on_frame)) {
        out->on_frame = &interactiveFrame;
    }
#ifdef TC_CLOCK_DRIVER
    tc_clock_driver::start(h);
#endif
    if (h->log) {
        const std::string note = std::string("clock: interactive pair ") +
            (g_board.valid() ? "armed" : "without a selection model") + "; mouse " +
            (g_mouseClicked && g_mouseReleased ? "ok" : "missing");
        h->log(h->context, note.c_str());
    }
    if (!tc::component_types::table(h, &typesApi)) {
        if (h->log) h->log(h->context, "clock: this loader has no tc.component.types");
        return 3;
    }
    static const TCComponentPinV2 outputPin{"out", "Clock", 1, 0};
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kClockId;
    definition.type_id = "local.clock/clock";
    definition.name = "时钟源";
    definition.description =
        "每 N 个周期翻转一次；N 写在左上角的小方框里，点一下方框可以输入数字或表达式"
        "（8、0x10、1+7），回车生效并随原理图保存。N=1 即原版每周期翻转。"
        "门级延迟模式下按单位输出脉冲。";
    definition.inputs = nullptr;
    definition.input_count = 0;
    definition.outputs = &outputPin;
    definition.output_count = 1;
    /* state[0] is the level, state[1] the cycles it has held it. */
    definition.state_words = 2;
    /* A source costs nothing to place and one unit to read: the pulse reaches a
       reader one unit after it appears. */
    definition.gate_cost = 0;
    definition.delay = 1;
    /* The period the player set, in the instance's own configuration: the
       registered default is the classic once-per-cycle clock, so an instance
       that never sees a click - and every board saved before this field
       existed - keeps exactly the behaviour it had. */
    static const ClockConfig clockDefaults{1, 0};
    definition.config_schema = kClockConfigSchema;
    definition.config_size = sizeof(ClockConfig);
    definition.default_config = &clockDefaults;
    definition.callback = &clockLogic;
    const int status = tc::component_types::registerDefinition(typesApi, &definition);
    if (status != TC_COMPONENT_TYPES_OK) {
        if (h->log)
            h->log(h->context, (std::string("clock: registration refused: ") +
                                tc::component_types::errorText(status)).c_str());
        return 4;
    }
    if (h->log) h->log(h->context, "clock: registered the clock source (0 in, 1 bit out)");

    /* The interactive pair.  Registration only gives them a place on the board and
       a host-owned configuration; the click handling that writes that
       configuration lives in the same Mod (next step). */
    struct Interactive {
        uint64_t id;
        const char* typeId;
        const char* name;
        const char* description;
        uint8_t momentary;
    };
    static const Interactive interactive[] = {
        {kSwitchId, "local.clock/switch", "自锁开关",
         "0 输入 1 位输出：运行中点一下翻转并锁定，用于手动给电平。", 0},
        {kButtonId, "local.clock/button", "按钮",
         "0 输入 1 位输出：按住为 1、松开为 0，用于手动给脉冲。", 1},
    };
    for (const Interactive& entry : interactive) {
        static const TCComponentPinV2 out{"out", "Level", 1, 0};
        TCComponentTypeDefinitionV2 type{};
        type.size = sizeof(type);
        type.version = TC_COMPONENT_TYPES_VERSION_2;
        type.custom_id = entry.id;
        type.type_id = entry.typeId;
        type.name = entry.name;
        type.description = entry.description;
        type.inputs = nullptr;
        type.input_count = 0;
        type.outputs = &out;
        type.output_count = 1;
        type.state_words = 0;
        type.gate_cost = 0;
        type.delay = 1;
        type.callback = &switchLogic;
        static const uint8_t defaults[sizeof(SwitchConfig)] = {0, 0};
        static const SwitchConfig switchDefaults{0, 0};
        static const SwitchConfig buttonDefaults{0, 1};
        type.config_schema = 1;
        type.config_size = sizeof(SwitchConfig);
        type.default_config = entry.momentary ? static_cast<const void*>(&buttonDefaults)
                                              : static_cast<const void*>(&switchDefaults);
        (void)defaults;
        const int registered = tc::component_types::registerDefinition(typesApi, &type);
        if (h->log)
            h->log(h->context, (std::string("clock: ") + entry.name +
                                (registered == TC_COMPONENT_TYPES_OK ? " registered"
                                                                    : " refused"))
                                   .c_str());
    }
    /* Without a footprint the game's own point query never lands on these types,
       so a click selects whatever sits behind them (measured: the selection
       changed while none of our ids was ever in it).  Declaring the board
       rectangle is what makes them clickable at all - the game then hit-tests,
       selects, drags and deletes them like any other part. */
    tc::component_geometry::Api geometry{};
    if (tc::component_geometry::table(h, &geometry)) {
        /* The face is painted inside this box, which stops short of the output
           pin at (2,0): the pin keeps its own drag area and the reserved space
           does not reach past it (see kFootprintHalfWidth). */
        tc::component_geometry::setFootprint(geometry, kClockId, kFootprintHalfWidth,
                                             kFootprintHalfHeight);
        tc::component_geometry::setFootprint(geometry, kSwitchId, 2.0f, 1.5f);
        tc::component_geometry::setFootprint(geometry, kButtonId, 2.0f, 1.5f);
        note("clock: component footprints set with output-pin lanes left clear");
    } else {
        note("clock: no geometry table; the interactive pair stays unclickable");
    }
    /* Own both board faces.  V2 suppresses the generic custom-component mesh
       while keeping output pins, selection and native board behaviour. */
    tc::component_render::ApiV2 render{};
    if (tc::component_render::tableV2(h, &render)) {
        bool allOwned = true;
        const int clockCallback =
            tc::component_render::setDrawCallback(render, kClockId, &renderClock);
        const int clockDefault = clockCallback == TC_COMPONENT_RENDER_OK
            ? tc::component_render::setDefaultDrawing(render, kClockId, false)
            : clockCallback;
        if (clockCallback != TC_COMPONENT_RENDER_OK ||
            clockDefault != TC_COMPONENT_RENDER_OK) {
            allOwned = false;
            note(std::string("clock: CLK UI takeover failed: callback=") +
                 tc::component_render::errorText(clockCallback) + " default=" +
                 tc::component_render::errorText(clockDefault));
        }
        const uint64_t interactiveIds[] = {kSwitchId, kButtonId};
        for (const uint64_t id : interactiveIds) {
            const int callbackStatus =
                tc::component_render::setDrawCallback(render, id, &renderInteractive);
            const int defaultStatus = callbackStatus == TC_COMPONENT_RENDER_OK
                ? tc::component_render::setDefaultDrawing(render, id, false)
                : callbackStatus;
            if (callbackStatus != TC_COMPONENT_RENDER_OK ||
                defaultStatus != TC_COMPONENT_RENDER_OK) {
                allOwned = false;
                note(std::string("clock: interactive UI takeover failed: callback=") +
                     tc::component_render::errorText(callbackStatus) + " default=" +
                     tc::component_render::errorText(defaultStatus));
            }
        }
        if (allOwned)
            note("clock: all component UI owned (blue CLK, blue button, purple switch)");
    } else {
        note("clock: component render V2 unavailable; interactive pair keeps game UI");
    }
    tc::component_render::ApiV4 renderV4{};
    if (tc::component_render::tableV4(h, &renderV4)) {
        (void)tc::component_render::setFoundryButton(renderV4, kClockId, false);
        (void)tc::component_render::setFoundryButton(renderV4, kSwitchId, false);
        (void)tc::component_render::setFoundryButton(renderV4, kButtonId, false);
    }
    return 0;
}
