/* Text box: a decorative component that draws a text note on the board.

   The component itself does nothing.  It is a board object the player places
   from the game's own component list, moves, copies and deletes exactly like
   any other component, and everything the player sees is drawn by this plugin
   over the board.

   Why a real component and not a plugin-side overlay: a component comes with
   placement, selection, move, rotate, copy/paste, undo and the save format for
   free, and the note stays where the player put it.

   The shape is 0 inputs + one *unconnected* output, and that is measured, not
   stylistic.  A true 0-in/0-out component registers and displays fine (see
   docs/research/text-component.md), but with no pin it is unreachable from the
   board's net: the compiler drops it, the loader never binds an instance, and
   tc.component.storage - which needs a bound instance to write the component's
   record - can never persist the text.  A floating output keeps the component
   decorative (nothing is connected, the callback drives 0, the declared cost
   is 0 gates / 0 delay) and the pin sits under the note's own box, so what the
   player sees is a pinless box.

   Because no instance is ever bound, the text lives in this Mod's own store
   (`<plugin-data>/notes.txt`), keyed by the board it belongs to plus the
   component's own 64-bit id.  Consequences the player can see: a note travels
   with the board across sessions, but a *copy* of a note inside the game gets a
   new id and therefore starts empty, and the text is not inside the schematic
   file when that file is shared.

   Geometry and drawing are the measured facts this file depends on; all of
   them are recorded in docs/research/text-component.md:

   * board -> screen is the game's own `world_pos_to_screen_pos`, whose result
     is normalised and multiplied by the ImGuiIO display size (the board-grid
     mod established that), so a note is drawn in board coordinates and follows
     pan and zoom;
   * ImDrawList::AddText with an explicit font size has no cimgui entry point in
     this build (`ImFont_RenderText`'s exported wrapper is not usable as a
     plain C call), but the draw list's shared data carries the font and the
     size it will use (`ImDrawList + 0x38`, `+0x18` and `+0x20`), read straight
     from ImDrawList_AddText_Vec2's own code.  The pair is swapped for the
     duration of one AddText call and put back, so nothing else in the frame
     sees it;
   * the game draws a custom component's own name as a watermark across the
     component, so the default background is nearly opaque and the box has a
     minimum size that covers it. */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_component_types.h"
#include "../../sdk/tc_component_geometry.h"
#include "../../sdk/tc_component_render.h"
#include "../../sdk/tc_board_model.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using tc::ui::Color;
using tc::ui::Vec2;
using tc::ui::Vec4;

struct V2 {
    float x, y;
};

inline Color rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255) {
    return Color(r) | (Color(g) << 8) | (Color(b) << 16) | (Color(a) << 24);
}

/* ---------------------------------------------------------------------------
   The component
   ------------------------------------------------------------------------ */

/* "TEXT_001": the id lives in every schematic the note is placed in, so it must
   never change. */
constexpr uint64_t kNoteId = 0x544558545F303031ULL;
constexpr uint64_t kHiddenProbeId = 0x544558545F483031ULL;  /* "TEXT_H01" */
constexpr uint64_t kVisibleProbeId = 0x544558545F563031ULL; /* "TEXT_V01" */
/* The note's type footprint, in board cells (tc.component.geometry takes half
   extents).  It remains the placement/occupancy box.  Geometry V2 overlays the
   exact per-instance visible size for native pointer interaction. */
constexpr float kFootprintHalfWidth = 4.f;
constexpr float kFootprintHalfHeight = 2.f;
/* A note never draws shorter than this, so an empty or one-line note still covers
   its footprint instead of collapsing into a thin bar. */
constexpr float kNoteMinHeightUnits = 3.6f;
/* where the dragged copy of the hidden type is placed, so the drag case can
   tell it apart from the copy that is only ever measured */
constexpr int32_t kDragProbeX = -6;
constexpr int32_t kDragProbeY = -18;
constexpr uint32_t kConfigSchema = 2;
constexpr uint32_t kCustomInstanceKind = 0x4e;

enum : uint32_t {
    kFlagBold = 1u << 0,
    kFlagItalic = 1u << 1,
    kFlagBorder = 1u << 2,
};

enum : uint32_t { kAlignLeft = 0, kAlignCentre = 1, kAlignRight = 2 };

/* One component instance's configuration.  This is the blob the loader copies
   per instance, writes into the component record and reads back when the
   schematic is loaded; the persisted form is capped at 1024 bytes. */
struct NoteConfig {
    uint32_t version;
    float fontUnits;     /* text height, in board units */
    float widthUnits;    /* box width, in board units */
    float heightUnits;   /* box height, in board units */
    float paddingUnits;  /* inner padding, in board units */
    uint32_t background; /* ImU32, alpha included */
    uint32_t foreground;
    uint32_t flags;
    uint32_t align;
    char text[480];      /* UTF-8, NUL terminated */
};
static_assert(sizeof(NoteConfig) <= 1024, "one persisted configuration is capped at 1024 bytes");

NoteConfig defaultConfig() {
    NoteConfig config{};
    config.version = kConfigSchema;
    /* Defaults fill the 8x4 footprint: 2 * kFootprintHalfWidth minus the padding on
       both sides, and a font that reads well at the board's own zoom. */
    config.fontUnits = 0.60f;
    config.widthUnits = 2.f * kFootprintHalfWidth - 0.4f;
    config.heightUnits = kNoteMinHeightUnits;
    config.paddingUnits = 0.22f;
    /* The game draws a custom component's own name as a watermark on the
       board; a nearly opaque background is what keeps it from showing through
       the note.  Lowering the alpha is allowed and brings it back. */
    config.background = rgba(22, 24, 33, 238);
    config.foreground = rgba(233, 235, 242, 255);
    config.flags = kFlagBorder;
    config.align = kAlignLeft;
    return config;
}

/* ---------------------------------------------------------------------------
   Engine entry points used by the board overlay
   ------------------------------------------------------------------------ */

const TCHost* host = nullptr;

void* (*igGetIO)() = nullptr;
void* (*igGetMainViewport)() = nullptr;
void* (*igGetBackgroundDrawList)(void*) = nullptr;
void* (*igGetForegroundDrawList)(void*) = nullptr;

/* Board -> normalised screen, the game's own camera transform. */
V2 (*worldToScreen)(V2) = nullptr;

/* ImVec2 is returned through a hidden pointer in this build (measured; the same
   convention the loader's own drawing table uses for igCalcTextSize). */
void (*fontCalcTextSize)(V2* out, const void* font, float size, float maxWidth,
                         float wrapWidth, const char* begin, const char* end,
                         const char** remaining) = nullptr;
const char* (*imFontDebugName)(const void*) = nullptr;

bool (*igColorEdit4)(const char* label, float* colour, int flags) = nullptr;
bool (*igInputTextMultiline)(const char* label, char* buffer, unsigned long long capacity,
                            V2 size, int flags, void* callback, void* user) = nullptr;
void (*igSetNextFrameWantCaptureMouse)(bool capture) = nullptr;
bool (*igIsAnyItemActiveOriginal)() = nullptr;

/* build_board_ui samples igIsAnyItemActive at this verified return address
   before it lets the circuit board consume the mouse.  The note overlay is
   drawn later in the frame, so a resize handle must carry its ownership into
   the next sample explicitly; WantCaptureMouse alone is not consulted by this
   board path. */
constexpr uintptr_t kBoardInputSampleReturnRva = 0x46b593;
bool resizeBoardInputRequested = false;
uint64_t resizeBoardInputBlocks = 0;
/* Every visit to the board's input sample site, and how many of them were *not*
   answered by the block: a gesture that still reaches the board has to show up as
   unblocked samples in the window it happened in. */
uint64_t boardSampleCalls = 0;
uint64_t boardSampleUnblocked = 0;
uint64_t boardSampleUnblockedInGesture = 0;
/* True while a node owns the drag (pressed and still down).  Published a frame
   late exactly like the block request, so a board sample can tell "the resize was
   running" from "the mouse happened to hover a node". */
bool resizeGestureActive = false;
/* Whether the frame's overlay pass could evaluate the handles at all.  The pass
   needs the background draw list and a successful board enumeration; a frame
   where either is missing must not silently drop the request (that is a hole the
   board's input sample can fall through). */
uint64_t nodePassCount = 0;
uint64_t nodePassSkipped = 0;
uint64_t leakedSampleCount = 0;
/* Whether the frame's overlay pass found the mouse on a note's handle (or on a
   live drag).  Read by the board's input sample one frame late, exactly like the
   request flag, so a leaked sample can say whether the pass before it claimed the
   mouse at all. */
bool resizePassClaimed = false;
/* TC_TEXTBOX_BLOCK_BOARD_INPUT=0 drops the block above and leaves only the ImGui
   hint: the sensitivity control for "the node gesture never reaches the board".
   With it off the same synthetic gesture is a board drag as well, which is what
   the gate's control run records (see the resize stage of the autotest). */
bool blockBoardInput = true;
/* Defined with report(): the autotest's census of every caller that asks whether
   an ImGui item is active. */
void censusAnyItemActiveCaller(uintptr_t rva);
extern bool autotest;
extern int autotestStage;
void report(const std::string& message);

bool hookIsAnyItemActive() {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (autotest && caller >= image) censusAnyItemActiveCaller(caller - image);
    if (caller >= image && caller - image == kBoardInputSampleReturnRva) {
        ++boardSampleCalls;
        if (resizeBoardInputRequested) {
            ++resizeBoardInputBlocks;
            return true;
        }
        ++boardSampleUnblocked;
        if (resizeGestureActive) ++boardSampleUnblockedInGesture;
        /* Only a leak while the button is down matters for the wire symptom: one
           that happens while the mouse merely moves onto a handle cannot start
           anything, one that happens mid-drag can. */
        if (autotest && resizeGestureActive && leakedSampleCount < 8) {
            ++leakedSampleCount;
            char line[192];
            std::snprintf(line, sizeof(line),
                          "text-box autotest: board sample leaked stage=%d tick=%llu "
                          "passes=%llu skipped=%llu gesture=%d pass-claimed=%d",
                          autotestStage, static_cast<unsigned long long>(GetTickCount64()),
                          static_cast<unsigned long long>(nodePassCount),
                          static_cast<unsigned long long>(nodePassSkipped),
                          resizeGestureActive ? 1 : 0, resizePassClaimed ? 1 : 0);
            report(line);
        }
    }
    return igIsAnyItemActiveOriginal ? igIsAnyItemActiveOriginal() : false;
}

/* ---------------------------------------------------------------------------
   The board's own mouse-state reads
   ------------------------------------------------------------------------

   Answering "an item is active" at the sample site above only steers which
   branch of handle_io_on_board runs - it is not the board's input gate.  Measured
   on the real machine: with every sample during a node drag answered "active", the
   board still committed a wire whose two endpoints are exactly the press and the
   release point of that drag.  The branch is reached from build_level_tree_ui's
   flags rather than from the sample answer, and it ends in handle_ongoing_action,
   which is where the wire came from.

   So the node also hides its gesture at the source.  handle_io_on_board reads the
   four mouse-button states once per frame, in a window at the top of the function
   (0x1403558c7..0x140355935), and packs them into its own io state; the rest of the
   board works from that packed copy.  While a note owns the mouse, those calls are
   answered "not pressed".  Only callers inside that window are affected - every
   other caller of the same ImGui functions gets the real answer, so the panels,
   menus and ImGui itself are untouched. */
constexpr uintptr_t kBoardMouseReadFirstRva = 0x3558b0;
constexpr uintptr_t kBoardMouseReadLastRva = 0x355940;

/* Both mouse-query shapes are forwarded as two integers: the button and the id
   the board passes (it passes zero).  Declaring both keeps the ABI safe whichever
   arity the target build uses. */
bool (*igIsMouseDownIdOriginal)(int, int) = nullptr;
bool (*igIsMouseClickedOriginal)(int, int) = nullptr;
bool (*igIsMouseDoubleClickedOriginal)(int, int) = nullptr;
bool (*igIsMouseReleasedOriginal)(int, int) = nullptr;
uint64_t boardMouseReadsHidden = 0;

bool boardMouseHidden(uintptr_t caller, uintptr_t image) {
    if (!resizeBoardInputRequested || !blockBoardInput || caller < image) return false;
    const uintptr_t rva = caller - image;
    if (rva < kBoardMouseReadFirstRva || rva > kBoardMouseReadLastRva) return false;
    ++boardMouseReadsHidden;
    return true;
}

bool hookIsMouseDownId(int button, int id) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (boardMouseHidden(caller, image)) return false;
    return igIsMouseDownIdOriginal ? igIsMouseDownIdOriginal(button, id) : false;
}

bool hookIsMouseClicked(int button, int flags) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (boardMouseHidden(caller, image)) return false;
    return igIsMouseClickedOriginal ? igIsMouseClickedOriginal(button, flags) : false;
}

bool hookIsMouseDoubleClicked(int button, int id) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (boardMouseHidden(caller, image)) return false;
    return igIsMouseDoubleClickedOriginal ? igIsMouseDoubleClickedOriginal(button, id) : false;
}

bool hookIsMouseReleased(int button, int id) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (boardMouseHidden(caller, image)) return false;
    return igIsMouseReleasedOriginal ? igIsMouseReleasedOriginal(button, id) : false;
}

/* The display size lives in ImGuiIO; the game's own draw_simple_rect multiplies
   the normalised transform below by it (the board-grid mod measured this). */
bool displaySize(V2* out) {
    if (!igGetIO || !out) return false;
    void* io = igGetIO();
    if (!io) return false;
    *out = *reinterpret_cast<V2*>(static_cast<unsigned char*>(io) + 8);
    return out->x > 8.f && out->y > 8.f;
}

/* The game's own font table: [1] is the bold face, [2] the regular one the
   panels and the board use, and both carry the CJK ranges the game's own text
   needs (the loader borrows the same table for plugin tool text). */
void* regularFont = nullptr;
void* boldFont = nullptr;

/* ---------------------------------------------------------------------------
   Services and per-instance state
   ------------------------------------------------------------------------ */

TCBoardApiV6 boardApi{};
TCComponentTypesApiV1 typesApi{};
TCComponentGeometryApiV1 geometryApi{};
TCComponentGeometryApiV2 geometryApiV2{};
TCComponentRenderApiV2 renderApi{};
/* The board overlay computes the exact visible box (including text-driven
   minimums).  The component render callback always has a fresh game handle, so
   it publishes that box as the live native interaction footprint. */
std::map<uint64_t, std::pair<float, float>> liveNoteHalfExtents;
/* The game's own selection set, so the note can draw a selection hint that
   follows its declared footprint instead of the component mesh (which ignores
   it).  Keys are board component sequence indices, see sdk/tc_board_model.h. */
tc::TCBoardModel boardSelection;
std::map<uint64_t, bool> selectedInstances;
std::set<uint64_t> hintReportedInstances;
/* Re-report the hint while it stays selected, so the last line before the
   capture proves the instance was still selected when the frame was taken. */
unsigned long long lastHintReportAt = 0;
/* TC_TEXTBOX_KEEP_ARCS=1 leaves the game's own arc on, the control run for the
   "did the arc disappear" pixel measurement. */
bool keepGameArcs = false;
bool hintOrange = false;
/* TC_TEXTBOX_LAYOUT_TRACE=1: report the note layout in board cells once a second. */
bool layoutTrace = false;
/* TC_TEXTBOX_FOUNDRY_BUTTON=1 keeps the game's "edit this component in the foundry"
   button on the component panel; the default is to drop it for this Mod's types. */
bool keepFoundryButton = false;
bool renderCallbackSeen = false;
bool autotest = false;
bool defaultDrawingProbe = false;
/* The default-drawing case ends with a drag so the pixels it measures cannot be
   an artefact of a hover or selection highlight.  TC_TEXTBOX_DRAG_PROBE=0
   leaves the drag out, which is how the two effects were told apart. */
bool dragProbe = true;
/* TC_TEXTBOX_KEEP_SELECTION=1 leaves the dragged instance selected, so the
   captured frame keeps whatever selection hint the game draws for it.  Used to
   measure that hint; the normal run clears the selection. */
bool keepSelection = false;
/* TC_TEXTBOX_FOREGROUND_TEST=1 paints an opaque patch over the last selection
   hint on the foreground draw list, to measure which list sits above the game's
   own selection arcs. */
bool foregroundTest = false;
bool haveHintBox = false;
Vec2 hintBoxMin{0.f, 0.f}, hintBoxMax{0.f, 0.f};
bool defaultProbeCentersReported = false;
uint32_t renderRotationMask = 0;
uint64_t renderValidatedFrames = 0;
bool renderTransformReported = false;
bool renderTransformReference = false;
bool renderZoomRequested = false;
bool renderZoomReported = false;
float renderInitialUnit = 0.f;
float renderBaseExX = 0.f, renderBaseExY = 0.f;
float renderBaseEyX = 0.f, renderBaseEyY = 0.f;
struct RenderSample {
    float originX = 0.f, originY = 0.f;
    float axisXX = 0.f, axisXY = 0.f, axisYX = 0.f, axisYY = 0.f;
};
std::map<uint64_t, RenderSample> renderPanBaseline;
std::map<uint64_t, RenderSample> renderPanCurrent;
bool renderPanBaselineReady = false;
bool renderPanRequested = false;
bool renderPanReported = false;
bool renderClipProbeReported = false;
void report(const std::string& message);

/* Autotest census of everyone who asks "is an ImGui item active?".  The board's
   own input sample is exactly one return address (0x46b593) and that is where the
   resize block answers; this reports every other entry point once, so a caller
   the block does not cover shows up in the log instead of silently letting a
   mouse gesture through. */
extern int autotestStage;   /* defined with the self-test below */
void censusAnyItemActiveCaller(uintptr_t rva) {
    static std::set<uintptr_t> seen;
    if (seen.size() >= 64 || !seen.insert(rva).second) return;
    char line[128];
    std::snprintf(line, sizeof(line),
                  "text-box autotest: igIsAnyItemActive caller rva=0x%llx stage=%d",
                  static_cast<unsigned long long>(rva), autotestStage);
    report(line);
}

bool closeFloat(float a, float b, float tolerance = 0.05f) {
    return std::abs(a - b) <= tolerance;
}

void renderNoteOutline(void*, const TCComponentRenderFrameV1* renderFrame) {
    if (!renderFrame) return;
    if (renderFrame->custom_id == kNoteId && geometryApiV2.set_instance_footprint) {
        const auto live = liveNoteHalfExtents.find(renderFrame->instance_id);
        if (live != liveNoteHalfExtents.end())
            tc::component_geometry::setInstanceFootprint(
                geometryApiV2, renderFrame->component, live->second.first, live->second.second);
    }
    if (!renderFrame->draw) return;
    const auto* draw = renderFrame->draw;
    const bool isNote = renderFrame->custom_id == kNoteId;
    float x[4]{}, y[4]{};
    tc::component_render::localToScreen(*renderFrame, -4.f, -2.f, &x[0], &y[0]);
    tc::component_render::localToScreen(*renderFrame, kFootprintHalfWidth, -kFootprintHalfHeight,
                                        &x[1], &y[1]);
    tc::component_render::localToScreen(*renderFrame, kFootprintHalfWidth, kFootprintHalfHeight,
                                        &x[2], &y[2]);
    tc::component_render::localToScreen(*renderFrame, -kFootprintHalfWidth, kFootprintHalfHeight,
                                        &x[3], &y[3]);
    /* The game's own selection hint is one sprite per selected element, built at a
       fixed scale, so it can follow neither the declared rectangle nor the aspect
       ratio of this type - V3 turns it off for the note instead
       (set_selection_hint(id,false)).  The note therefore draws its own hint: while
       the instance is selected, the declared 8x4 footprint - the same box the game
       uses for placement and dragging - gets a ring and a light fill, in a colour
       nothing else in this Mod uses, so a pixel readback can measure it. */
    const auto selected = selectedInstances.find(renderFrame->instance_id);
    const bool isSelected = selected != selectedInstances.end() && selected->second;
    /* The note itself draws a live box-sized selection range in the board
       overlay below, where its persisted per-instance width and height are
       available.  Probe types keep this fixed-footprint path for the geometry
       and clipping regression tests. */
    if (isSelected && !isNote) {
        float minX = x[0], maxX = x[0], minY = y[0], maxY = y[0];
        for (int corner = 1; corner < 4; ++corner) {
            minX = std::min(minX, x[corner]); maxX = std::max(maxX, x[corner]);
            minY = std::min(minY, y[corner]); maxY = std::max(maxY, y[corner]);
        }
        const bool axisAligned = std::abs(renderFrame->axis_x_y) < 0.01f &&
                                 std::abs(renderFrame->axis_y_x) < 0.01f;
        /* Same visual language as the game's own hint - a white ring - but drawn
           on the declared footprint instead of the component mesh.  Orange is
           kept for the diagnostic measurement (TC_TEXTBOX_HINT_ORANGE=1). */
        const Color ring = hintOrange ? rgba(255, 128, 0, 235) : rgba(255, 255, 255, 225);
        const Color inner = hintOrange ? rgba(255, 128, 0, 140) : rgba(255, 255, 255, 120);
        const Color fill = hintOrange ? rgba(255, 128, 0, 28) : rgba(255, 255, 255, 26);
        if (axisAligned)
            draw->rect_filled(draw->context, minX, minY, maxX, maxY, fill, 3.f);
        hintBoxMin = {minX, minY};
        hintBoxMax = {maxX, maxY};
        haveHintBox = true;
        for (int edge = 0; edge < 4; ++edge) {
            draw->line(draw->context, x[edge], y[edge], x[(edge + 1) & 3],
                       y[(edge + 1) & 3], ring, 2.5f);
            const float innerX0 = x[edge] + (x[(edge + 1) & 3] - x[edge]) * 0.04f;
            const float innerY0 = y[edge] + (y[(edge + 1) & 3] - y[edge]) * 0.04f;
            const float innerX1 = x[(edge + 1) & 3] + (x[edge] - x[(edge + 1) & 3]) * 0.04f;
            const float innerY1 = y[(edge + 1) & 3] + (y[edge] - y[(edge + 1) & 3]) * 0.04f;
            draw->line(draw->context, innerX0, innerY0, innerX1, innerY1, inner, 1.25f);
        }
        const unsigned long long now = GetTickCount64();
        if (now - lastHintReportAt > 500) {
            lastHintReportAt = now;
            const float unit = std::sqrt(renderFrame->axis_x_x * renderFrame->axis_x_x +
                                         renderFrame->axis_x_y * renderFrame->axis_x_y);
            char line[256];
            std::snprintf(line, sizeof(line),
                          "PASS text-box selection hint instance=%llu "
                          "centre=%.1f,%.1f half=%.2f,%.2f box=%.1f,%.1f..%.1f,%.1f unit=%.3f rotation=%u",
                          static_cast<unsigned long long>(renderFrame->instance_id),
                          renderFrame->origin_x, renderFrame->origin_y,
                          static_cast<double>(kFootprintHalfWidth),
                          static_cast<double>(kFootprintHalfHeight),
                          minX, minY, maxX, maxY, unit, renderFrame->rotation & 3u);
            report(line);
        }
    }
    /* Do not leave the old fixed 8x4 diagnostic rectangle visible in the
       middle of a resizable note. */
    if (!isNote)
        for (int edge = 0; edge < 4; ++edge)
            draw->line(draw->context, x[edge], y[edge], x[(edge + 1) & 3], y[(edge + 1) & 3],
                       rgba(255, 255, 255, 72), 1.25f);
    if (!renderCallbackSeen) {
        renderCallbackSeen = true;
        report("text-box: tc.component.render callback active (host affine basis + clipped primitives)");
    }
    if (!autotest) return;
    const bool diagnosticClip = closeFloat(renderFrame->clip_min_x, 800.f) &&
                                closeFloat(renderFrame->clip_min_y, 200.f) &&
                                closeFloat(renderFrame->clip_max_x, 1800.f) &&
                                closeFloat(renderFrame->clip_max_y, 850.f);
    /* The true-game clipping run supplies an inset diagnostic clip.  Four
       opaque markers cross its four edges; the playtest reads the framebuffer
       and proves that only the intersection made it to pixels.  Keep drawing
       them every frame so the delayed in-process screenshot sees them. */
    if ((renderFrame->rotation & 3u) == 0u && diagnosticClip) {
        draw->rect_filled(draw->context, 768.f, 240.f, 832.f, 272.f,
                          rgba(251, 1, 197), 0.f);       /* left: magenta */
        draw->rect_filled(draw->context, 1768.f, 280.f, 1832.f, 312.f,
                          rgba(253, 211, 3), 0.f);       /* right: yellow */
        draw->rect_filled(draw->context, 880.f, 168.f, 912.f, 232.f,
                          rgba(2, 227, 251), 0.f);       /* top: cyan */
        draw->rect_filled(draw->context, 920.f, 818.f, 952.f, 882.f,
                          rgba(7, 249, 83), 0.f);        /* bottom: green */
        if (!renderClipProbeReported) {
            renderClipProbeReported = true;
            report("text-box autotest: clip pixel markers armed rect=800,200..1800,850");
        }
    }
    const float liveUnit = std::sqrt(renderFrame->axis_x_x * renderFrame->axis_x_x +
                                     renderFrame->axis_x_y * renderFrame->axis_x_y);
    if (renderTransformReported && renderZoomRequested && !renderZoomReported &&
        std::isfinite(liveUnit) && std::abs(liveUnit - renderInitialUnit) > 0.5f) {
        renderZoomReported = true;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "PASS text-box render zoom live unit %.2f -> %.2f",
                      renderInitialUnit, liveUnit);
        report(line);
    }
    const RenderSample sample{renderFrame->origin_x, renderFrame->origin_y,
                              renderFrame->axis_x_x, renderFrame->axis_x_y,
                              renderFrame->axis_y_x, renderFrame->axis_y_y};
    if (renderZoomReported && !renderPanRequested && renderPanBaseline.size() < 4) {
        renderPanBaseline[renderFrame->instance_id] = sample;
        if (renderPanBaseline.size() == 4) {
            renderPanBaselineReady = true;
            report("text-box autotest: captured four render origins before camera pan");
        }
    }
    if (renderPanRequested && !renderPanReported) {
        const auto baseline = renderPanBaseline.find(renderFrame->instance_id);
        if (baseline != renderPanBaseline.end()) renderPanCurrent[renderFrame->instance_id] = sample;
        if (renderPanBaselineReady && renderPanCurrent.size() == renderPanBaseline.size()) {
            bool consistent = true;
            bool haveDelta = false;
            float referenceDx = 0.f, referenceDy = 0.f;
            for (const auto& entry : renderPanBaseline) {
                const auto current = renderPanCurrent.find(entry.first);
                if (current == renderPanCurrent.end()) { consistent = false; break; }
                const float dx = current->second.originX - entry.second.originX;
                const float dy = current->second.originY - entry.second.originY;
                if (!haveDelta) {
                    referenceDx = dx; referenceDy = dy; haveDelta = true;
                }
                consistent = consistent && closeFloat(dx, referenceDx, 0.35f) &&
                             closeFloat(dy, referenceDy, 0.35f) &&
                             closeFloat(current->second.axisXX, entry.second.axisXX, 0.05f) &&
                             closeFloat(current->second.axisXY, entry.second.axisXY, 0.05f) &&
                             closeFloat(current->second.axisYX, entry.second.axisYX, 0.05f) &&
                             closeFloat(current->second.axisYY, entry.second.axisYY, 0.05f);
            }
            if (consistent && haveDelta &&
                std::sqrt(referenceDx * referenceDx + referenceDy * referenceDy) > 2.f) {
                renderPanReported = true;
                char line[224];
                std::snprintf(line, sizeof(line),
                              "PASS text-box render pan delta=%.2f,%.2f axes unchanged instances=%u",
                              referenceDx, referenceDy,
                              static_cast<unsigned>(renderPanBaseline.size()));
                report(line);
            }
        }
    }
    if (renderTransformReported) return;

    /* Recover the unrotated board basis from every quarter-turn.  If the host
       accidentally ignores rotation (or swaps/signs one axis incorrectly),
       the four recovered pairs disagree and the PASS line never appears. */
    float exX = renderFrame->axis_x_x, exY = renderFrame->axis_x_y;
    float eyX = renderFrame->axis_y_x, eyY = renderFrame->axis_y_y;
    switch (renderFrame->rotation & 3u) {
    case 1: {
        const float oldExX = exX, oldExY = exY;
        exX = -eyX; exY = -eyY; eyX = oldExX; eyY = oldExY;
        break;
    }
    case 2: exX = -exX; exY = -exY; eyX = -eyX; eyY = -eyY; break;
    case 3: {
        const float oldExX = exX, oldExY = exY;
        exX = eyX; exY = eyY; eyX = -oldExX; eyY = -oldExY;
        break;
    }
    default: break;
    }
    const float exLength = std::sqrt(exX * exX + exY * exY);
    const float eyLength = std::sqrt(eyX * eyX + eyY * eyY);
    if (!std::isfinite(exLength) || !std::isfinite(eyLength) || exLength <= 1.f ||
        eyLength <= 1.f || renderFrame->clip_max_x <= renderFrame->clip_min_x ||
        renderFrame->clip_max_y <= renderFrame->clip_min_y)
        return;
    if (!renderTransformReference) {
        renderTransformReference = true;
        renderBaseExX = exX; renderBaseExY = exY;
        renderBaseEyX = eyX; renderBaseEyY = eyY;
    }
    if (!closeFloat(exX, renderBaseExX) || !closeFloat(exY, renderBaseExY) ||
        !closeFloat(eyX, renderBaseEyX) || !closeFloat(eyY, renderBaseEyY))
        return;
    bool clipped = false;
    for (int corner = 0; corner < 4; ++corner) {
        float cornerX = 0.f, cornerY = 0.f;
        tc::component_render::localToScreen(
            *renderFrame, (corner & 1) ? 4.f : -4.f, (corner & 2) ? 2.f : -2.f,
            &cornerX, &cornerY);
        clipped = clipped || cornerX < renderFrame->clip_min_x ||
                  cornerX > renderFrame->clip_max_x || cornerY < renderFrame->clip_min_y ||
                  cornerY > renderFrame->clip_max_y;
    }
    /* In the diagnostic run some placed instances intentionally cross the
       inset clip.  Their axes are still valid transform evidence; the actual
       clipping result is checked later from framebuffer pixels. */
    if (clipped && !diagnosticClip) return;
    renderRotationMask |= 1u << (renderFrame->rotation & 3u);
    ++renderValidatedFrames;
    if (renderRotationMask == 0x0fu && renderValidatedFrames >= 4) {
        renderTransformReported = true;
        renderInitialUnit = exLength;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "PASS text-box render transforms rotations=0x%x unit=%.2f/%.2f clip=%.0fx%.0f",
                      renderRotationMask, exLength, eyLength,
                      renderFrame->clip_max_x - renderFrame->clip_min_x,
                      renderFrame->clip_max_y - renderFrame->clip_min_y);
        report(line);
    }
}

/* Notes live in this Mod's own store rather than in the component's
   configuration blob, and that is a measured limitation rather than a choice:
   a decorative component is not reachable from the board's net, so the game's
   compiler drops it, the loader never binds an instance for it, and
   tc.component.storage - which needs a bound instance - can never write its
   record.  The store is keyed by the board it belongs to plus the component's
   own 64-bit id, which the schematic file preserves. */
std::map<std::string, NoteConfig> store;
bool storeDirty = false;
unsigned long long storeSavedAt = 0;
std::string storePath;
std::string boardKey = "board";
uint64_t editing = 0;                   /* instance whose editor is open */
Vec2 editingOrigin{0.f, 0.f};
std::map<uint64_t, unsigned long long> lastClick; /* double-click detection, per note */
bool measuredOnce = false;
uint32_t registeredOutputs = 0;         /* 0 for a true 0-in/0-out component */

struct ResizeState {
    uint64_t instance = 0;
    int xDirection = 0;
    int yDirection = 0;
    bool changed = false;
};
ResizeState resizing;

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

void note(const std::string& message, int level) {
    if (host && host->report_status) host->report_status(host->context, level, message.c_str());
}

/* ---------------------------------------------------------------------------
   Configuration helpers
   --------------------------------------------------------------------------- */

void colorToFloats(uint32_t colour, float* out) {
    out[0] = static_cast<float>(colour & 0xFFu) / 255.f;
    out[1] = static_cast<float>((colour >> 8) & 0xFFu) / 255.f;
    out[2] = static_cast<float>((colour >> 16) & 0xFFu) / 255.f;
    out[3] = static_cast<float>((colour >> 24) & 0xFFu) / 255.f;
}

uint32_t floatsToColor(const float* value) {
    const auto quantise = [](float channel) {
        const int scaled = static_cast<int>(channel * 255.f + 0.5f);
        return static_cast<unsigned char>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
    };
    return rgba(quantise(value[0]), quantise(value[1]), quantise(value[2]), quantise(value[3]));
}

std::string noteKey(uint64_t instance) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(instance));
    return boardKey + "/" + buffer;
}

NoteConfig& configFor(uint64_t instance) {
    const std::string key = noteKey(instance);
    auto found = store.find(key);
    if (found == store.end()) found = store.emplace(key, defaultConfig()).first;
    return found->second;
}

void markDirty(uint64_t instance) {
    (void)instance;
    storeDirty = true;
}

std::vector<std::string> splitLines(const char* text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char* at = text ? text : ""; *at; ++at) {
        if (*at == '\r') continue;
        if (*at == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(*at);
        }
    }
    lines.push_back(current);
    return lines;
}

/* ---------------------------------------------------------------------------
   Text measurement and drawing
   ------------------------------------------------------------------------ */

float measureLine(const std::string& line, float size) {
    if (line.empty() || !fontCalcTextSize || !regularFont) return 0.f;
    V2 measured{0.f, 0.f};
    fontCalcTextSize(&measured, regularFont, size, 4096.f, 0.f, line.c_str(),
                     line.c_str() + line.size(), nullptr);
    if (!std::isfinite(measured.x) || measured.x <= 0.f || measured.x > 100000.f) return 0.f;
    return measured.x;
}

/* The font and the font size ImDrawList::AddText will use live in the draw
   list's shared data; see the file header for where those offsets come from.
   Both fields are restored before anything else runs. */
struct FontOverride {
    void* drawList = nullptr;
    unsigned char* shared = nullptr;
    void* savedFont = nullptr;
    float savedSize = 0.f;

    FontOverride(void* list, void* font, float size) {
        if (!list || !font || !(size > 0.f)) return;
        auto* bytes = static_cast<unsigned char*>(list);
        unsigned char* data = *reinterpret_cast<unsigned char**>(bytes + 0x38);
        if (!data) return;
        drawList = list;
        shared = data;
        savedFont = *reinterpret_cast<void**>(data + 0x18);
        savedSize = *reinterpret_cast<float*>(data + 0x20);
        *reinterpret_cast<void**>(data + 0x18) = font;
        *reinterpret_cast<float*>(data + 0x20) = size;
    }
    ~FontOverride() {
        if (!shared) return;
        *reinterpret_cast<void**>(shared + 0x18) = savedFont;
        *reinterpret_cast<float*>(shared + 0x20) = savedSize;
    }
    FontOverride(const FontOverride&) = delete;
    FontOverride& operator=(const FontOverride&) = delete;
};

/* One line at one size.  Italic has no face in the game's font set, so it is
   synthesised: the line is drawn once per horizontal band, each band shifted
   sideways by the slant at that height and clipped to itself.  The bands are
   one pixel tall, so the steps stay sub-pixel and the result reads as a slanted
   face rather than a staircase. */
void drawTextLine(void* list, const std::string& line, bool bold, bool italic, Vec2 position,
                  float size, Color colour, float width) {
    auto& api = tc::ui::drawing_detail::table();
    if (!api.text || line.empty()) return;
    void* font = bold && boldFont ? boldFont : regularFont;
    if (!font) return;
    FontOverride override_(list, font, size);
    if (!italic) {
        api.text(list, position, colour, line.c_str(), line.c_str() + line.size());
        return;
    }
    constexpr float kSlant = 0.22f;
    const float height = std::max(4.f, size * 1.25f);
    const float reach = kSlant * height * 0.5f + 2.f;
    for (float offset = 0.f; offset < height; offset += 1.f) {
        const float shift = kSlant * (height * 0.5f - (offset + 0.5f));
        const Vec2 minimum{position.x - reach + shift, position.y + offset};
        const Vec2 maximum{position.x + width + reach + shift, position.y + offset + 1.f};
        api.pushClip(list, minimum, maximum, true);
        api.text(list, Vec2{position.x + shift, position.y}, colour, line.c_str(),
                 line.c_str() + line.size());
        api.popClip(list);
    }
}

/* ---------------------------------------------------------------------------
   Board geometry
   ------------------------------------------------------------------------ */

struct BoardMap {
    bool valid = false;
    Vec2 origin{0.f, 0.f};
    Vec2 ex{0.f, 0.f};
    Vec2 ey{0.f, 0.f};
    float unit = 0.f;   /* pixels per board unit along y */
};

BoardMap boardMap() {
    BoardMap map;
    if (!worldToScreen) return map;
    V2 display{0.f, 0.f};
    if (!displaySize(&display)) return map;
    const V2 origin = worldToScreen({0.f, 0.f});
    const V2 unitX = worldToScreen({1.f, 0.f});
    const V2 unitY = worldToScreen({0.f, 1.f});
    map.origin = {origin.x * display.x, origin.y * display.y};
    map.ex = {unitX.x * display.x - map.origin.x, unitX.y * display.y - map.origin.y};
    map.ey = {unitY.x * display.x - map.origin.x, unitY.y * display.y - map.origin.y};
    map.unit = std::sqrt(map.ey.x * map.ey.x + map.ey.y * map.ey.y);
    map.valid = std::isfinite(map.unit) && map.unit > 1.f && map.unit < 4000.f &&
                std::isfinite(map.ex.x) && std::isfinite(map.ex.y);
    return map;
}

Vec2 toScreen(const BoardMap& map, float x, float y) {
    return {map.origin.x + x * map.ex.x + y * map.ey.x,
            map.origin.y + x * map.ex.y + y * map.ey.y};
}

/* True while a board is on screen; the grid's own test: the loaded level name
   is a Nim string whose length is zero everywhere else. */
const void* loadedLevelName = nullptr;
const void* schematicPathSlot = nullptr;
void* (*emutlsAddress)(void*) = nullptr;

/* Whichever string names the board on screen: the level being played, or the
   schematic being edited inside the component workshop. */
std::string nimString(const void* address) {
    struct NimStringHeader {
        int64_t length;
        const char* data;
    };
    if (!address) return std::string();
    const auto* header = static_cast<const NimStringHeader*>(address);
    if (header->length <= 0 || header->length > 512 || !header->data) return std::string();
    return std::string(header->data, static_cast<size_t>(header->length));
}

/* The default-drawing probe centres, in screen pixels.  The gate measures its pixel
   windows from the line this prints, so it is printed again whenever the camera has
   moved (the arc zoom-out probe moves it after the first report). */
void reportProbeCenters(const BoardMap& map) {
    const Vec2 hidden = toScreen(map, -12.f, -12.f);
    const Vec2 visible = toScreen(map, 12.f, -12.f);
    char line[224];
    std::snprintf(line, sizeof(line),
                  "PASS text-box default drawing probe centers hidden=%.1f,%.1f visible=%.1f,%.1f",
                  hidden.x, hidden.y, visible.x, visible.y);
    report(line);
}

void refreshBoardKey() {
    std::string key = nimString(loadedLevelName);
    if (key.empty() && schematicPathSlot && emutlsAddress) {
        void* slot = emutlsAddress(const_cast<void*>(schematicPathSlot));
        key = nimString(slot);
    }
    if (key.empty()) key = "board";
    if (key != boardKey) boardKey = key;
}

bool boardUp() {
    if (!loadedLevelName) return true;
    struct NimStringHeader {
        int64_t length;
        const void* data;
    };
    const auto* name = static_cast<const NimStringHeader*>(loadedLevelName);
    return name->length > 0 && name->length <= 64 && name->data != nullptr;
}

/* ---------------------------------------------------------------------------
   The note on the board
   ------------------------------------------------------------------------ */

struct NoteOnBoard {
    uint64_t instance = 0;
    int32_t x = 0, y = 0;
    uint32_t rotation = 0;
    /* where the component sits in the board's own component sequence: the
       game's selection set is keyed by that index, not by the record id */
    uint64_t index = 0;
    TCGameHandle handle{};
};

std::vector<TCGameHandle> componentHandles;
/* Diagnostics: what the board enumeration actually handed back (logged at a low
   rate while the drawing is on, so a live run can be read afterwards). */
struct ScanStats {
    int captureStatus = -999;
    int firstStatus = -999;
    uint64_t countFromQuery = 0xFFFFFFFFull;
    uint64_t components = 0;
    uint64_t customKind = 0;
    uint64_t matched = 0;
    int source = 0;   /* 1 = tc.board, 2 = the board tables read directly */
};
ScanStats lastScan{};

/* The Board object tables, exactly as the loader reads them itself
   (src/board_objects.hpp): component sequence length/payload at +0x78/+0x80,
   records start after an 8-byte header with a 0x238 stride; a record carries
   kind +0x00, schematic x/y +0x02/+0x04, its own id +0x08 and the custom
   prototype id +0x188.  This is the fallback for the case where the tc.board
   service answers with an empty table for the board that is actually on
   screen. */
/* `wantedId` is the custom prototype the caller looks for: the note's own type
   for drawing, or a probe type for the default-drawing case.  `stats` is null
   for the probe scans so they cannot disturb the low-rate diagnostic line. */
bool collectNotesDirect(const void* board, uint64_t wantedId, std::vector<NoteOnBoard>& out,
                        ScanStats* stats) {
    if (!board) return false;
    const auto* bytes = static_cast<const unsigned char*>(board);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, bytes + 0x78, sizeof(count));
    std::memcpy(&data, bytes + 0x80, sizeof(data));
    if (!data || count == 0 || count > 1000000ull) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(data, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_NOACCESS))
        return false;
    const unsigned char* first = data + 8;
    uint64_t custom = 0, matched = 0;
    for (uint64_t index = 0; index < count; ++index) {
        const unsigned char* record = first + index * 0x238ull;
        if (record[0] != kCustomInstanceKind) continue;
        ++custom;
        uint64_t prototype = 0, id = 0;
        int16_t x = 0, y = 0;
        std::memcpy(&prototype, record + 0x188, sizeof(prototype));
        if (prototype != wantedId) continue;
        std::memcpy(&id, record + 0x08, sizeof(id));
        std::memcpy(&x, record + 0x02, sizeof(x));
        std::memcpy(&y, record + 0x04, sizeof(y));
        ++matched;
        out.push_back(NoteOnBoard{id, x, y, record[0x06], index, {}});
    }
    if (stats) {
        stats->source = 2;
        stats->components = count;
        stats->customKind = custom;
        stats->matched = matched;
    }
    return true;
}

bool collectBoardInstances(uint64_t wantedId, std::vector<NoteOnBoard>& out, ScanStats* stats) {
    out.clear();
    if (stats) *stats = ScanStats{};
    if (!boardApi.get_current || !boardApi.capture_objects || !boardApi.read_component) return false;
    TCGameHandle board{};
    const int currentStatus = boardApi.get_current(boardApi.context, &board);
    if (currentStatus != TC_HANDLE_OK) {
        if (stats) {
            stats->captureStatus = 1000;
            stats->firstStatus = currentStatus;
        }
        const void* raw = nullptr;
        return tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) == TC_HANDLE_OK &&
               boardApi.resolve(boardApi.context, &board, &raw) == TC_HANDLE_OK &&
               collectNotesDirect(raw, wantedId, out, stats);
    }
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    int status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (stats) {
        stats->firstStatus = status;
        stats->countFromQuery = snapshot.component_count;
    }
    if (status == TC_SNAPSHOT_ERR_CAPACITY) {
        componentHandles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
        buffers.components = componentHandles.data();
        buffers.component_capacity = componentHandles.size();
        status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    }
    if (stats) {
        stats->captureStatus = status;
        stats->components = snapshot.component_written;
    }
    if (status != TC_SNAPSHOT_OK || snapshot.component_count == 0) {
        const void* raw = nullptr;
        if (boardApi.resolve(boardApi.context, &board, &raw) == TC_HANDLE_OK &&
            collectNotesDirect(raw, wantedId, out, stats))
            return true;
        if (status != TC_SNAPSHOT_OK) return false;
    }
    if (stats) stats->source = 1;
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        const TCGameHandle& component = componentHandles[index];
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (boardApi.read_component(boardApi.context, &component, &info,
                                    static_cast<uint32_t>(sizeof(info))) != TC_SNAPSHOT_OK)
            continue;
        /* kind 0 is a tombstone left behind by an undo, not an element. */
        if (info.kind != kCustomInstanceKind) continue;
        if (stats) ++stats->customKind;
        if (!(info.flags & TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE)) continue;
        if (info.custom_prototype_id != wantedId) continue;
        if (stats) ++stats->matched;
        out.push_back(NoteOnBoard{info.id, info.x, info.y, info.rotation, index, component});
    }
    return true;
}

bool collectNotes(std::vector<NoteOnBoard>& out) {
    return collectBoardInstances(kNoteId, out, &lastScan);
}

/* The same enumeration for one probe type.  Used by the default-drawing case,
   which needs the hidden instance's board position. */
bool collectInstancesOf(uint64_t wantedId, std::vector<NoteOnBoard>& out) {
    return collectBoardInstances(wantedId, out, nullptr);
}

/* Count-only board object query.  "Dragging a resize node also draws a wire" is a
   statement about what the board did with the mouse, and the board's own object
   counts are the observable form of it: a note resize must leave both at zero
   change.  Zero capacities are the documented count-only call - it answers
   CAPACITY and still reports both counts. */
bool boardObjectCounts(uint64_t* components, uint64_t* wires) {
    if (!boardApi.get_current || !boardApi.capture_objects) return false;
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return false;
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    const int status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                                static_cast<uint32_t>(sizeof(snapshot)),
                                                &buffers);
    if (status != TC_SNAPSHOT_OK && status != TC_SNAPSHOT_ERR_CAPACITY) return false;
    if (components) *components = snapshot.component_count;
    if (wires) *wires = snapshot.wire_count;
    return true;
}

/* Autotest diagnostic: where every wire on the board actually is.  "The board
   gained a wire" is only useful with the endpoints, which say whether it came
   from the gesture under test or was already part of the level. */
void reportWireInventory() {
    if (!boardApi.get_current || !boardApi.capture_objects || !boardApi.read_wire) return;
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return;
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    /* Count-only first call answers CAPACITY while still reporting both counts. */
    int status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (status != TC_SNAPSHOT_OK && status != TC_SNAPSHOT_ERR_CAPACITY) return;
    if (snapshot.wire_count == 0 || snapshot.wire_count > 64) return;
    /* The snapshot is all-or-nothing: both buffers have to have room, so the
       component set is allocated even though only the wires are read here. */
    std::vector<TCGameHandle> components(static_cast<size_t>(snapshot.component_count));
    std::vector<TCGameHandle> wires(static_cast<size_t>(snapshot.wire_count));
    buffers.components = components.empty() ? nullptr : components.data();
    buffers.component_capacity = components.size();
    buffers.wires = wires.data();
    buffers.wire_capacity = wires.size();
    status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                     static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (status != TC_SNAPSHOT_OK) return;
    std::string line = "text-box autotest: wire inventory";
    for (size_t index = 0; index < wires.size(); ++index) {
        TCWireInfoV1 info{};
        info.size = sizeof(info);
        info.version = TC_WIRE_INFO_VERSION_1;
        if (boardApi.read_wire(boardApi.context, &wires[index], &info,
                               static_cast<uint32_t>(sizeof(info))) != TC_SNAPSHOT_OK)
            continue;
        line += " [" + std::to_string(index) + "] " + std::to_string(info.x1) + "," +
                std::to_string(info.y1) + "->" + std::to_string(info.x2) + "," +
                std::to_string(info.y2) + " width=" + std::to_string(info.bit_width);
    }
    report(line);
}

/* One note's box, in pixels. */
struct NoteBox {
    Vec2 minimum{}, maximum{};
    Vec2 buttonMinimum{}, buttonMaximum{};
    float textSize = 12.f;
    float lineHeight = 15.f;
    float padding = 4.f;
    float textTop = 0.f;
    bool placeholder = false;
    std::vector<std::string> lines;
    std::vector<float> widths;
};

NoteBox layoutNote(const NoteConfig& config, float unit, Vec2 centre) {
    NoteBox box;
    /* Everything here is proportional to `unit` (screen pixels per board cell), with
       only a sub-pixel floor to keep the values usable: an absolute floor of a few
       pixels is what used to freeze the text, the padding and the edit button while
       the board itself kept shrinking. */
    box.textSize = std::max(0.5f, config.fontUnits * unit);
    box.lineHeight = box.textSize * 1.30f;
    box.padding = std::max(0.5f, config.paddingUnits * unit);
    box.placeholder = config.text[0] == 0;
    box.lines = splitLines(box.placeholder ? "空文本框：点右上角编辑" : config.text);
    if (box.lines.empty()) box.lines.emplace_back();

    float widest = 0.f;
    box.widths.reserve(box.lines.size());
    for (const std::string& line : box.lines) {
        const float width = measureLine(line, box.textSize);
        box.widths.push_back(width);
        widest = std::max(widest, width);
    }
    const float minimumWidth = std::max(1.f, config.widthUnits * unit);
    const float boxWidth = std::max(minimumWidth, widest + 2.f * box.padding);
    /* The floor keeps a single short line covering the game's own name
       watermark, which is drawn at the component's centre. */
    const float minimumHeight = std::max(1.f, config.heightUnits * unit);
    const float boxHeight = std::max(minimumHeight,
                                     box.lineHeight * static_cast<float>(box.lines.size()) +
                                         2.f * box.padding);
    box.minimum = {centre.x - boxWidth * 0.5f, centre.y - boxHeight * 0.5f};
    box.maximum = {centre.x + boxWidth * 0.5f, centre.y + boxHeight * 0.5f};
    /* A note is an editing surface, not a label: keep its content attached to
       the top content edge as the user grows the box.  Vertically centring the
       line block made a resized note look as though the text had stayed behind
       at the component origin. */
    box.textTop = box.minimum.y + box.padding;
    const float button = std::max(2.f, unit * 0.62f);
    const float buttonInset = std::max(0.4f, unit * 0.07f);
    box.buttonMaximum = {box.maximum.x - buttonInset,
                         box.minimum.y + buttonInset + button};
    box.buttonMinimum = {box.buttonMaximum.x - button, box.buttonMaximum.y - button};
    return box;
}

void drawPencil(void* list, const NoteBox& box, Color colour) {
    auto& api = tc::ui::drawing_detail::table();
    if (!api.line) return;
    const float size = box.buttonMaximum.x - box.buttonMinimum.x;
    const float stroke = std::max(0.8f, size * 0.13f);
    const Vec2 tip{box.buttonMinimum.x + size * 0.24f, box.buttonMaximum.y - size * 0.24f};
    const Vec2 head{box.buttonMaximum.x - size * 0.24f, box.buttonMinimum.y + size * 0.24f};
    api.line(list, tip, head, colour, stroke);
    const Vec2 back{tip.x - size * 0.10f, tip.y + size * 0.10f};
    api.line(list, tip, back, colour, stroke);
}

bool drawNote(void* list, const BoardMap& map, const NoteOnBoard& note, const NoteConfig& config,
              bool hovered) {
    auto& api = tc::ui::drawing_detail::table();
    if (!api.rectFilled || !api.text) return false;
    const Vec2 centre = toScreen(map, static_cast<float>(note.x), static_cast<float>(note.y));
    const NoteBox box = layoutNote(config, map.unit, centre);
    const float rounding = std::max(1.f, map.unit * 0.16f);
    api.rectFilled(list, box.minimum, box.maximum, config.background, rounding, 0);
    if (config.flags & kFlagBorder)
        api.rect(list, box.minimum, box.maximum, rgba(255, 255, 255, hovered ? 120 : 60),
                 rounding, 0, std::max(0.6f, map.unit * 0.05f));

    const bool bold = (config.flags & kFlagBold) != 0;
    const bool italic = (config.flags & kFlagItalic) != 0;
    const float reserved = (box.buttonMaximum.x - box.buttonMinimum.x) + map.unit * 0.14f;
    const float availableText = std::max(1.f, box.maximum.x - reserved - box.minimum.x -
                                                2.f * box.padding);
    const Color textColour = box.placeholder ? rgba(200, 205, 220, 130) : config.foreground;
    for (size_t i = 0; i < box.lines.size(); ++i) {
        const std::string& line = box.lines[i];
        if (line.empty()) continue;
        const float width = box.widths[i];
        float x = box.minimum.x + box.padding;
        if (config.align == kAlignCentre)
            x += std::max(0.f, (availableText - width) * 0.5f);
        else if (config.align == kAlignRight)
            x += std::max(0.f, availableText - width);
        const float y = box.textTop + static_cast<float>(i) * box.lineHeight;
        drawTextLine(list, line, bold, italic, Vec2{x, y}, box.textSize, textColour, width);
    }

    api.rectFilled(list, box.buttonMinimum, box.buttonMaximum,
                   hovered ? rgba(255, 255, 255, 70) : rgba(255, 255, 255, 28), rounding, 0);
    drawPencil(list, box, hovered ? rgba(255, 255, 255, 235) : rgba(255, 255, 255, 150));
    return true;
}

/* The selection affordance follows the actual rendered note, including content
   growth and live handle resizing.  Geometry V2 gives the same rectangle to the
   game's native point lookup without enlarging the type's placement footprint. */
void drawLiveSelectionRange(void* list, const BoardMap& map, const NoteOnBoard& note,
                            const NoteBox& box, bool selected) {
    if (!list || !selected) return;
    auto& draw = tc::ui::drawing_detail::table();
    if (!draw.rect) return;
    const Color outer = hintOrange ? rgba(255, 128, 0, 235) : rgba(255, 255, 255, 225);
    const Color inner = hintOrange ? rgba(255, 128, 0, 140) : rgba(255, 255, 255, 115);
    const float rounding = std::max(1.f, map.unit * 0.16f);
    draw.rect(list, box.minimum, box.maximum, outer, rounding, 0,
              std::max(1.5f, map.unit * 0.09f));
    const float inset = std::max(1.f, map.unit * 0.07f);
    draw.rect(list, {box.minimum.x + inset, box.minimum.y + inset},
              {box.maximum.x - inset, box.maximum.y - inset}, inner,
              std::max(0.f, rounding - inset), 0, std::max(0.8f, map.unit * 0.04f));

    hintBoxMin = box.minimum;
    hintBoxMax = box.maximum;
    haveHintBox = true;
    static unsigned long long lastLiveRangeReportAt = 0;
    const unsigned long long now = GetTickCount64();
    if (now - lastLiveRangeReportAt > 500) {
        lastLiveRangeReportAt = now;
        const float unit = std::max(1.f, map.unit);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "PASS text-box live selection range instance=%llu "
                      "half=%.2f,%.2f box=%.1f,%.1f..%.1f,%.1f unit=%.3f",
                      static_cast<unsigned long long>(note.instance),
                      (box.maximum.x - box.minimum.x) * 0.5f / unit,
                      (box.maximum.y - box.minimum.y) * 0.5f / unit,
                      box.minimum.x, box.minimum.y, box.maximum.x, box.maximum.y, unit);
        report(line);
    }
}

/* The eight direct-manipulation handles of one note, in a fixed order: corners
   resize both axes, edge nodes resize one.  The component stays centred because
   the game does not yet expose an instance move+resize transaction; the visible
   box updates continuously. */
struct NoteHandles {
    Vec2 centre[8];
    int xDirection[8];
    int yDirection[8];
};

NoteHandles noteHandles(const NoteBox& box) {
    const float midX = (box.minimum.x + box.maximum.x) * 0.5f;
    const float midY = (box.minimum.y + box.maximum.y) * 0.5f;
    const float xs[8] = {box.minimum.x, midX,         box.maximum.x, box.maximum.x,
                         box.maximum.x, midX,         box.minimum.x, box.minimum.x};
    const float ys[8] = {box.minimum.y, box.minimum.y, box.minimum.y, midY,
                         box.maximum.y, box.maximum.y, box.maximum.y, midY};
    const int xDirections[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
    const int yDirections[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
    NoteHandles handles{};
    for (int index = 0; index < 8; ++index) {
        handles.centre[index] = {xs[index], ys[index]};
        handles.xDirection[index] = xDirections[index];
        handles.yDirection[index] = yDirections[index];
    }
    return handles;
}

int hoveredHandleIndex(const NoteBox& box, Vec2 mouse, float unit) {
    const NoteHandles handles = noteHandles(box);
    const float hitRadius = std::max(7.f, unit * 0.28f);
    int hovered = -1;
    float closest = hitRadius * hitRadius;
    for (int index = 0; index < 8; ++index) {
        const float dx = mouse.x - handles.centre[index].x;
        const float dy = mouse.y - handles.centre[index].y;
        const float distance = dx * dx + dy * dy;
        if (distance <= closest) {
            closest = distance;
            hovered = index;
        }
    }
    return hovered;
}

/* Everything the node does with the mouse, in one place: claim the mouse from the
   board's input sample, adopt a press that landed on a handle, and keep resizing
   while the button stays down.  This runs for every note on every frame, before
   drawing and independently of the draw list: a frame in which the handles cannot
   be drawn (no foreground list, a failed board enumeration) must still keep the
   board's hands off the mouse.  Dropping the claim for a single frame is enough
   for the board to take the press as its own and start a wire on the node. */
bool noteTakesMouse(const NoteOnBoard& note, NoteConfig& config, const NoteBox& box,
                    float unit, Vec2 mouse, bool enabled, bool selected) {
    const bool active = resizing.instance == note.instance;
    if (!enabled && !active) return false;
    const int hoveredHandle = hoveredHandleIndex(box, mouse, unit);
    const bool claimed = active || (selected && hoveredHandle >= 0);
    if (!claimed) return false;
    if (blockBoardInput) resizeBoardInputRequested = true;
    if (igSetNextFrameWantCaptureMouse) igSetNextFrameWantCaptureMouse(true);
    if (active) resizeGestureActive = true;
    if (!active && tc::ui::isMouseClicked(tc::ui::Mouse_Left)) {
        const NoteHandles handles = noteHandles(box);
        resizing.instance = note.instance;
        resizing.xDirection = handles.xDirection[hoveredHandle];
        resizing.yDirection = handles.yDirection[hoveredHandle];
        resizing.changed = false;
    }
    if (resizing.instance != note.instance) return true;
    if (!tc::ui::isMouseDown(tc::ui::Mouse_Left)) {
        resizing = {};
        return true;
    }
    const float unitNow = std::max(1.f, unit);
    const float midX = (box.minimum.x + box.maximum.x) * 0.5f;
    const float midY = (box.minimum.y + box.maximum.y) * 0.5f;
    float width = config.widthUnits;
    float height = config.heightUnits;
    if (resizing.xDirection != 0)
        width = std::clamp(2.f * std::abs(mouse.x - midX) / unitNow, 1.5f, 40.f);
    if (resizing.yDirection != 0)
        height = std::clamp(2.f * std::abs(mouse.y - midY) / unitNow, 1.f, 24.f);
    if (std::abs(width - config.widthUnits) > 0.001f ||
        std::abs(height - config.heightUnits) > 0.001f) {
        config.widthUnits = width;
        config.heightUnits = height;
        config.version = kConfigSchema;
        resizing.changed = true;
        markDirty(note.instance);
    }
    return true;
}

/* Drawing only: the input side lives in noteTakesMouse(), so this never decides
   whether the board may see the mouse. */
bool drawResizeHandles(void* list, const BoardMap& map, const NoteOnBoard& note,
                       const NoteBox& box, Vec2 mouse, bool selected) {
    if (!list || (!selected && resizing.instance != note.instance)) return false;
    auto& draw = tc::ui::drawing_detail::table();
    if (!draw.circleFilled) return false;

    const NoteHandles handles = noteHandles(box);
    const float nodeRadius = std::max(3.5f, map.unit * 0.14f);
    const int hoveredHandle = hoveredHandleIndex(box, mouse, map.unit);

    for (int index = 0; index < 8; ++index) {
        const bool thisActive = resizing.instance == note.instance &&
                                resizing.xDirection == handles.xDirection[index] &&
                                resizing.yDirection == handles.yDirection[index];
        const bool thisHovered = index == hoveredHandle;
        const Color fill = thisActive ? rgba(255, 190, 72, 255)
                           : thisHovered ? rgba(255, 255, 255, 255)
                                         : rgba(45, 49, 63, 255);
        const Color ring = thisActive ? rgba(255, 214, 128, 255)
                           : thisHovered ? rgba(130, 214, 255, 255)
                                         : rgba(230, 235, 245, 230);
        draw.circleFilled(list, handles.centre[index], nodeRadius, fill, 12);
        if (draw.circle)
            draw.circle(list, handles.centre[index], nodeRadius, ring, 12,
                        std::max(1.f, map.unit * 0.055f));
    }
    return hoveredHandle >= 0 || resizing.instance == note.instance;
}

/* ---------------------------------------------------------------------------
   The Mod's own store
   ------------------------------------------------------------------------ */

std::string escapeText(const char* text) {
    std::string out;
    for (const char* at = text; at && *at; ++at) {
        if (*at == '\\') out += "\\\\";
        else if (*at == '\n') out += "\\n";
        else if (*at == '\r') continue;
        else out.push_back(*at);
    }
    return out;
}

std::string unescapeText(const std::string& value) {
    std::string out;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 1 < value.size()) {
            const char next = value[++index];
            out.push_back(next == 'n' ? '\n' : next);
        } else {
            out.push_back(value[index]);
        }
    }
    return out;
}

void loadStore() {
    store.clear();
    if (storePath.empty()) return;
    std::ifstream file(storePath, std::ios::binary);
    if (!file) return;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t firstSpace = line.find(' ');
        if (firstSpace == std::string::npos) continue;
        const std::string key = line.substr(0, firstSpace);
        NoteConfig config = defaultConfig();
        char text[sizeof(config.text)]{};
        unsigned flags = 0, align = 0, version = 1;
        if (std::sscanf(line.c_str() + firstSpace + 1, "%u", &version) != 1) continue;
        int fields = 0;
        if (version >= 2) {
            fields = std::sscanf(line.c_str() + firstSpace + 1,
                                 "%u %f %f %f %f %u %u %u %u %479[^\n]",
                                 &version, &config.fontUnits, &config.widthUnits,
                                 &config.heightUnits, &config.paddingUnits, &config.background,
                                 &config.foreground, &flags, &align, text);
        } else {
            fields = std::sscanf(line.c_str() + firstSpace + 1,
                                 "%u %f %f %f %u %u %u %u %479[^\n]",
                                 &version, &config.fontUnits, &config.widthUnits,
                                 &config.paddingUnits, &config.background,
                                 &config.foreground, &flags, &align, text);
        }
        const int required = version >= 2 ? 9 : 8;
        const int textField = version >= 2 ? 10 : 9;
        if (fields >= required) {
            config.version = kConfigSchema;
            config.flags = flags;
            config.align = align;
            const std::string decoded = unescapeText(fields >= textField ? text : "");
            std::snprintf(config.text, sizeof(config.text), "%s", decoded.c_str());
            store[key] = config;
        }
    }
}

void saveStore() {
    if (storePath.empty()) return;
    std::ofstream file(storePath, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "# text-box notes: <board>/<component id> <schema> <font> <width> <height> <padding> "
            "<background> <foreground> <flags> <align> <text>\n";
    for (const auto& entry : store) {
        const NoteConfig& config = entry.second;
        file << entry.first << ' ' << kConfigSchema << ' ' << config.fontUnits << ' '
             << config.widthUnits << ' ' << config.heightUnits << ' ' << config.paddingUnits << ' '
             << config.background << ' ' << config.foreground << ' ' << config.flags << ' '
             << config.align << ' ' << escapeText(config.text) << '\n';
    }
    storeDirty = false;
    storeSavedAt = GetTickCount64();
}

void beginEditing(uint64_t instance, Vec2 origin) {
    editing = instance;
    editingOrigin = origin;
}

void endEditing() {
    if (editing) saveStore();
    editing = 0;
}

/* ---------------------------------------------------------------------------
   The editor
   ------------------------------------------------------------------------ */

void drawEditor() {
    if (!editing) return;
    NoteConfig& config = configFor(editing);
    const Vec2 size{470.f, 560.f};
    Vec2 position{editingOrigin.x + 24.f, editingOrigin.y - 40.f};
    V2 display{0.f, 0.f};
    if (displaySize(&display)) {
        position.x = std::min(position.x, display.x - size.x - 16.f);
        position.y = std::max(16.f, std::min(position.y, display.y - size.y - 16.f));
    }
    auto window = tc::ui::panel("文本框##text-box-editor", nullptr, size, position, 0.62f,
                                tc::ui::Window_None, tc::ui::Cond_Appearing);
    if (!window) return;
    if (!tc::ui::isWindowFocused(0)) {
        /* Clicking the board (or another window) closes the editor and commits
           the session as one undo step. */
        endEditing();
        return;
    }

    bool changed = false;
    tc::ui::text("文字内容（可多行，支持中文）");
    char buffer[sizeof(NoteConfig::text)];
    std::memcpy(buffer, config.text, sizeof(buffer));
    buffer[sizeof(buffer) - 1] = 0;
    if (igInputTextMultiline && igInputTextMultiline("##note-text", buffer, sizeof(buffer),
                                                    V2{-1.f, 150.f}, 0, nullptr, nullptr)) {
        std::memcpy(config.text, buffer, sizeof(config.text));
        config.text[sizeof(config.text) - 1] = 0;
        changed = true;
    }

    tc::ui::separator();
    float fontUnits = config.fontUnits;
    tc::ui::setNextItemWidth(240.f);
    if (tc::ui::sliderFloat("字号（板面单位）", &fontUnits, 0.10f, 2.00f, "%.2f")) {
        config.fontUnits = fontUnits;
        changed = true;
    }
    float widthUnits = config.widthUnits;
    tc::ui::setNextItemWidth(240.f);
    if (tc::ui::sliderFloat("最小宽度（板面单位）", &widthUnits, 0.5f, 40.f, "%.1f")) {
        config.widthUnits = widthUnits;
        changed = true;
    }
    float heightUnits = config.heightUnits;
    tc::ui::setNextItemWidth(240.f);
    if (tc::ui::sliderFloat("最小高度（板面单位）", &heightUnits, 1.f, 24.f, "%.1f")) {
        config.heightUnits = heightUnits;
        changed = true;
    }
    tc::ui::textDisabled("也可选中文本框，直接拖动边框上的节点调整大小");

    bool bold = (config.flags & kFlagBold) != 0;
    if (tc::ui::checkbox("加粗", &bold)) {
        config.flags = bold ? (config.flags | kFlagBold) : (config.flags & ~kFlagBold);
        changed = true;
    }
    tc::ui::sameLine();
    bool italic = (config.flags & kFlagItalic) != 0;
    if (tc::ui::checkbox("斜体", &italic)) {
        config.flags = italic ? (config.flags | kFlagItalic) : (config.flags & ~kFlagItalic);
        changed = true;
    }
    tc::ui::sameLine();
    bool border = (config.flags & kFlagBorder) != 0;
    if (tc::ui::checkbox("边框", &border)) {
        config.flags = border ? (config.flags | kFlagBorder) : (config.flags & ~kFlagBorder);
        changed = true;
    }

    tc::ui::text("对齐");
    tc::ui::sameLine();
    if (tc::ui::radioButton("左", config.align == kAlignLeft)) {
        config.align = kAlignLeft;
        changed = true;
    }
    tc::ui::sameLine();
    if (tc::ui::radioButton("中", config.align == kAlignCentre)) {
        config.align = kAlignCentre;
        changed = true;
    }
    tc::ui::sameLine();
    if (tc::ui::radioButton("右", config.align == kAlignRight)) {
        config.align = kAlignRight;
        changed = true;
    }

    if (igColorEdit4) {
        float foreground[4];
        colorToFloats(config.foreground, foreground);
        if (igColorEdit4("文字颜色", foreground, 0)) {
            config.foreground = floatsToColor(foreground);
            changed = true;
        }
        float background[4];
        colorToFloats(config.background, background);
        if (igColorEdit4("背景颜色（含透明度）", background, 0)) {
            config.background = floatsToColor(background);
            changed = true;
        }
    }

    tc::ui::separator();
    if (tc::ui::keys::pressed(tc::ui::Key_Escape)) {
        endEditing();
        return;
    }
    if (tc::ui::button("完成", {120.f, 0.f})) endEditing();
    tc::ui::sameLine();
    if (tc::ui::smallButton("恢复默认样式")) {
        const NoteConfig defaults = defaultConfig();
        const std::string text = config.text;
        config = defaults;
        std::snprintf(config.text, sizeof(config.text), "%s", text.c_str());
        changed = true;
    }
    tc::ui::sameLine();
    tc::ui::textDisabled("删除用 Del");

    if (changed) {
        markDirty(editing);
        if (config.version != kConfigSchema) config.version = kConfigSchema;
    }
}

/* ---------------------------------------------------------------------------
   Per-frame overlay
   ------------------------------------------------------------------------ */

/* Development self-test, the same shape the other local Mods use: a file in
   the Mod's own data directory names a level, and the plugin then places one
   note, types into it and reports what came back.  Inert without the file. */
int autotestStage = 0;
uint64_t autotestInstance = 0;
unsigned long long autotestStageStart = 0;
TCCommandApiV2 commandApi{};
Vec2 lastButtonCentre{0.f, 0.f};   /* where the edit button was drawn last frame */
bool haveLastButton = false;
Vec2 resizeProbeStart{0.f, 0.f};
Vec2 resizeProbeEnd{0.f, 0.f};
Vec2 selectionProbePoint{0.f, 0.f};
float resizeProbeWidth = 0.f;
float resizeProbeHeight = 0.f;
int32_t resizeProbeBoardX = 0;
int32_t resizeProbeBoardY = 0;
uint64_t resizeProbeInputBlocks = 0;
/* The board's own object counts before the node gesture: unchanged afterwards is
   the number behind "the drag did not reach the board". */
uint64_t resizeProbeComponents = 0;
uint64_t resizeProbeWires = 0;
bool resizeProbeCountsKnown = false;
uint64_t resizeProbeSampleBase = 0;
uint64_t resizeProbeUnblockedBase = 0;

/* One line per step of the resize gesture: the board's own object counts, and the
   input samples it got since the gesture started, split into the ones the node
   answered and the ones that leaked through.  "The board gained a wire" is not
   actionable without knowing at which step and with how many leaked samples. */
void resizeStep(const char* what) {
    uint64_t components = 0, wires = 0;
    const bool known = boardObjectCounts(&components, &wires);
    report(std::string("text-box autotest: resize step ") + what + " components=" +
           std::to_string(known ? components : 0) + " wires=" +
           std::to_string(known ? wires : 0) + " samples=" +
           std::to_string(boardSampleCalls - resizeProbeSampleBase) + " blocked=" +
           std::to_string(resizeBoardInputBlocks - resizeProbeInputBlocks) + " leaked=" +
           std::to_string(boardSampleUnblocked - resizeProbeUnblockedBase));
}
/* The default-drawing case drags the hidden probe: where it was when the drag
   started, the spot inside its empty box that was grabbed, and its instance. */
uint64_t hiddenDragInstance = 0;
NoteOnBoard hiddenDragStart{};
Vec2 hiddenDragGrab{0.f, 0.f};
/* TC_TEXTBOX_ARC_ZOOM_OUT=<notches> zooms the board out once the arc case has
   selected its note, so the same probe measures the game's arc at a second camera
   scale (diagnostic only). */
int arcZoomOutNotches = 0;
bool arcZoomOutDone = false;
/* The game's own "clear the selection" entry (a COFF symbol, the same one the
   hit-box research used).  The drag case clears the selection when it is done
   so the captured frame carries no hover or selection highlight. */
void (*clearSelections)() = nullptr;

HWND gameWindow() {
    struct Finder { DWORD process; HWND found; } finder{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& finder = *reinterpret_cast<Finder*>(data);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != finder.process || !IsWindowVisible(window)) return TRUE;
            RECT client{};
            if (!GetClientRect(window, &client) || client.right < 320 || client.bottom < 240)
                return TRUE;
            finder.found = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&finder));
    return finder.found;
}

bool cameraPanMouse(Vec2 point, int action) {
    const HWND window = gameWindow();
    if (!window) return false;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    const LPARAM packed = MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y));
    if (action == 0) {
        PostMessageW(window, WM_MOUSEMOVE, 0, packed);
        return PostMessageW(window, WM_MBUTTONDOWN, MK_MBUTTON, packed) != FALSE;
    }
    if (action == 1)
        return PostMessageW(window, WM_MOUSEMOVE, MK_MBUTTON, packed) != FALSE;
    return PostMessageW(window, WM_MBUTTONUP, 0, packed) != FALSE;
}

/* Posting real mouse messages is how the other local Mods drive the game from
   a test: the window is hidden but its message loop still delivers them.
   Press and release go out in separate calls because a down/up pair posted in
   one frame can be collapsed before the game samples the button: the editor
   click missed about one run in six while it was a single message burst. */
void clickAtScreenPoint(Vec2 point) {
    const HWND window = gameWindow();
    if (!window) return;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    const LPARAM packed = MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y));
    PostMessageW(window, WM_MOUSEMOVE, 0, packed);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, packed);
}

void releaseClickAtScreenPoint(Vec2 point) {
    const HWND window = gameWindow();
    if (!window) return;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    const LPARAM packed = MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y));
    PostMessageW(window, WM_MOUSEMOVE, 0, packed);
    PostMessageW(window, WM_LBUTTONUP, 0, packed);
}

/* `notches` is signed: positive zooms in, negative zooms out.  Diagnostic use only
   (TC_TEXTBOX_ARC_ZOOM_OUT) - the arc probe compares the game's own selection hint
   at two camera scales to find out whether that sprite scales with the board. */
void zoomAtScreenPoint(Vec2 point, int notches) {
    struct Finder { DWORD process; HWND found; } finder{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& finder = *reinterpret_cast<Finder*>(data);
            DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
            if (owner != finder.process || !IsWindowVisible(window)) return TRUE;
            RECT client{};
            if (!GetClientRect(window, &client) || client.right < 320 || client.bottom < 240)
                return TRUE;
            finder.found = window; return FALSE;
        }, reinterpret_cast<LPARAM>(&finder));
    if (!finder.found) return;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(finder.found, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(finder.found, WM_MOUSEMOVE, 0,
                 MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y)));
    for (int notch = 0; notch < std::abs(notches); ++notch)
        PostMessageW(finder.found, WM_MOUSEWHEEL,
                     MAKEWPARAM(0, static_cast<WORD>(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA)),
                     MAKELPARAM(screen.x, screen.y));
    renderZoomRequested = true;
    report("text-box autotest: requested board zoom " + std::to_string(notches) + " notch(es) at " +
           std::to_string(static_cast<int>(point.x)) + "," +
           std::to_string(static_cast<int>(point.y)));
}

void zoomAtScreenPoint(Vec2 point) { zoomAtScreenPoint(point, 1); }

/* A left-button drag on the board, posted the same way the camera pan is: the
   press, the move and the release land in different frames, so the game samples
   the button state instead of collapsing it into one message. */
bool boardDragMouse(Vec2 point, int action) {
    const HWND window = gameWindow();
    if (!window) return false;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    const LPARAM packed = MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y));
    if (action == 0) {
        PostMessageW(window, WM_MOUSEMOVE, 0, packed);
        return PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, packed) != FALSE;
    }
    if (action == 1) return PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, packed) != FALSE;
    return PostMessageW(window, WM_LBUTTONUP, 0, packed) != FALSE;
}

/* Move the pointer without pressing anything: the game follows it with a hover
   highlight and the pin-name labels, so the drag case parks it on empty board
   before the frame that gets captured. */
void hoverMouse(Vec2 point) {
    const HWND window = gameWindow();
    if (!window) return;
    POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(window, WM_MOUSEMOVE, 0,
                 MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y)));
}


void runAutotest(const std::vector<NoteOnBoard>& notes, const BoardMap& map) {
    if (!autotest) return;
    const unsigned long long now = GetTickCount64();
    if (autotestStage == 0 && commandApi.submit && boardApi.get_current) {
        TCGameHandle board{};
        if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return;
        TCCommandV2 place{};
        place.size = sizeof(place);
        place.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
        place.subject = board;
        place.custom_prototype_id = kNoteId;
        place.kind = kCustomInstanceKind;
        static const int positions[4][2] = {{-8, -5}, {8, -5}, {-8, 7}, {8, 7}};
        for (uint32_t rotation = 0; rotation < 4; ++rotation) {
            place.x = positions[rotation][0];
            place.y = positions[rotation][1];
            place.rotation = rotation;
            uint64_t request = 0;
            const int status = commandApi.submit(commandApi.context, &place, &request);
            report("text-box autotest: place rotation=" + std::to_string(rotation) +
                   " status=" + std::to_string(status) + " request=" +
                   std::to_string(request));
        }
        if (defaultDrawingProbe) {
            /* The hidden type is placed twice: the first instance is only ever
               measured (it must stay free of game pixels), the second one is
               the one the drag case grabs, so a highlight left behind by the
               drag cannot land inside the measured window. */
            struct ProbePlacement { uint64_t id; int x; int y; const char* name; };
            static const ProbePlacement probes[] = {
                {kHiddenProbeId, -12, -12, "hidden"},
                {kVisibleProbeId, 12, -12, "visible"},
                {kHiddenProbeId, kDragProbeX, kDragProbeY, "hidden-drag"}};
            for (const auto& probe : probes) {
                place.custom_prototype_id = probe.id;
                place.x = probe.x;
                place.y = probe.y;
                place.rotation = 0;
                uint64_t request = 0;
                const int status = commandApi.submit(commandApi.context, &place, &request);
                report("text-box autotest: place default-" + std::string(probe.name) +
                       " status=" + std::to_string(status) + " request=" +
                       std::to_string(request));
            }
        }
        autotestStage = 1;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 1 && notes.size() >= 4) {
        const auto target = std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& note) {
            return note.rotation == 0;
        });
        if (target == notes.end()) return;
        autotestInstance = target->instance;
        NoteConfig& config = configFor(autotestInstance);
        std::snprintf(config.text, sizeof(config.text),
                      "板面说明示例 Text note 123\n第二行：中文与 Latin\nthird line italics");
        config.fontUnits = 0.5f;
        config.widthUnits = 7.f;
        config.flags |= kFlagBold;
        markDirty(autotestInstance);
        report("text-box autotest: placed instance=0x" + std::to_string(autotestInstance) +
               " at board (" + std::to_string(target->x) + "," +
               std::to_string(target->y) + "), unit=" + std::to_string(map.unit) + "px");
        autotestStage = 2;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 2 && now - autotestStageStart > 1500 &&
        !storeDirty) {
        const std::string key = noteKey(autotestInstance);
        loadStore();
        const auto found = store.find(key);
        const bool ok = found != store.end();
        const NoteConfig readback = ok ? found->second : NoteConfig{};
        report(std::string("text-box autotest: configuration read back ") + (ok ? "ok" : "failed") +
               " text=\"" + (ok ? std::string(readback.text) : std::string()) +
               "\" font=" + std::to_string(ok ? readback.fontUnits : 0.f) +
               " bold=" + std::to_string(ok && (readback.flags & kFlagBold) ? 1 : 0));
        zoomAtScreenPoint({1750.f, 800.f});
        autotestStage = 3;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 3 && now - autotestStageStart > 700 && editing == 0) {
        if (!renderZoomReported) return;
        if (!renderPanBaselineReady) return;
        renderPanCurrent.clear();
        renderPanRequested = true;
        if (!cameraPanMouse({1750.f, 800.f}, 0)) {
            renderPanRequested = false;
            return;
        }
        report("text-box autotest: started middle-mouse camera drag at 1750,800");
        autotestStage = 4;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 4 && now - autotestStageStart > 250) {
        cameraPanMouse({1900.f, 800.f}, 1);
        report("text-box autotest: moved middle-mouse camera drag to 1900,800");
        autotestStage = 5;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 5 && now - autotestStageStart > 250) {
        cameraPanMouse({1900.f, 800.f}, 2);
        report("text-box autotest: released middle-mouse camera drag");
        autotestStage = 6;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 6 && now - autotestStageStart > 300 && editing == 0) {
        if (!renderPanReported) return;
        if (defaultDrawingProbe && !defaultProbeCentersReported) {
            defaultProbeCentersReported = true;
            reportProbeCenters(map);
        }
        if (!haveLastButton) return;
        clickAtScreenPoint(lastButtonCentre);
        report("text-box autotest: clicked the edit button at " +
               std::to_string(static_cast<int>(lastButtonCentre.x)) + "," +
               std::to_string(static_cast<int>(lastButtonCentre.y)));
        autotestStage = 7;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 7 && now - autotestStageStart > 250) {
        releaseClickAtScreenPoint(lastButtonCentre);
        autotestStage = 8;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 8 && now - autotestStageStart > 600) {
        report(std::string("text-box autotest: ") +
               (editing == autotestInstance ? "the click opened the editor"
                                            : "the click did NOT open the editor"));
        autotestStage = 80;
        autotestStageStart = now;
        return;
    }
    /* Select-and-drag the note's bottom-right resize node.  This is a real mouse
       gesture against the direct-manipulation path, not a write to the config. */
    if (autotestStage == 80 && now - autotestStageStart > 400) {
        if (editing) {
            endEditing();
            autotestStageStart = now;
            return;
        }
        const auto target = std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& note) {
            return note.instance == autotestInstance;
        });
        if (target == notes.end()) return;
        NoteConfig& config = configFor(autotestInstance);
        const Vec2 centre = toScreen(map, static_cast<float>(target->x),
                                     static_cast<float>(target->y));
        const NoteBox box = layoutNote(config, map.unit, centre);
        resizeProbeStart = box.maximum;
        resizeProbeEnd = {box.maximum.x + map.unit * 2.f,
                          box.maximum.y + map.unit};
        resizeProbeWidth = config.widthUnits;
        resizeProbeHeight = config.heightUnits;
        resizeProbeBoardX = target->x;
        resizeProbeBoardY = target->y;
        resizeProbeInputBlocks = resizeBoardInputBlocks;
        resizeProbeSampleBase = boardSampleCalls;
        resizeProbeUnblockedBase = boardSampleUnblocked;
        resizeProbeCountsKnown = boardObjectCounts(&resizeProbeComponents, &resizeProbeWires);
        hoverMouse(resizeProbeStart);
        resizeStep("hover");
        report("text-box autotest: hovered the bottom-right resize node");
        autotestStage = 81;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 81 && now - autotestStageStart > 250) {
        boardDragMouse(resizeProbeStart, 0);
        resizeStep("press");
        autotestStage = 82;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 82 && now - autotestStageStart > 300) {
        boardDragMouse(resizeProbeEnd, 1);
        resizeStep("move");
        autotestStage = 83;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 83 && now - autotestStageStart > 300) {
        boardDragMouse(resizeProbeEnd, 2);
        resizeStep("release");
        autotestStage = 84;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 84 && now - autotestStageStart > 500) {
        const NoteConfig& config = configFor(autotestInstance);
        const auto target = std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& note) {
            return note.instance == autotestInstance;
        });
        const bool stayedPut = target != notes.end() && target->x == resizeProbeBoardX &&
                               target->y == resizeProbeBoardY;
        const uint64_t blockedInputs = resizeBoardInputBlocks - resizeProbeInputBlocks;
        /* The board's own object counts around the gesture.  A wire drawn by the
           board would show up here even if the note happened not to move, and the
           control run below is what turns "0" into a measurement. */
        uint64_t componentsAfter = 0, wiresAfter = 0;
        const bool countsKnown =
            resizeProbeCountsKnown && boardObjectCounts(&componentsAfter, &wiresAfter);
        const long long componentDelta =
            countsKnown ? static_cast<long long>(componentsAfter) -
                              static_cast<long long>(resizeProbeComponents)
                        : 0;
        const long long wireDelta =
            countsKnown ? static_cast<long long>(wiresAfter) -
                              static_cast<long long>(resizeProbeWires)
                        : 0;
        const bool boardUntouched = countsKnown && componentDelta == 0 && wireDelta == 0;
        const uint64_t gestureSamples = boardSampleCalls - resizeProbeSampleBase;
        const uint64_t gestureLeaks = boardSampleUnblocked - resizeProbeUnblockedBase;
        resizeStep("settle");
        bool textFollowsTop = false;
        if (target != notes.end()) {
            const Vec2 centre = toScreen(map, static_cast<float>(target->x),
                                         static_cast<float>(target->y));
            const NoteBox resizedBox = layoutNote(config, map.unit, centre);
            textFollowsTop = std::abs(resizedBox.textTop -
                                      (resizedBox.minimum.y + resizedBox.padding)) < 0.1f;
        }
        const bool resized = config.widthUnits > resizeProbeWidth + 2.f &&
                             config.heightUnits > resizeProbeHeight + 0.4f;
        if (!blockBoardInput) {
            /* TC_TEXTBOX_BLOCK_BOARD_INPUT=0: the same synthetic gesture with the
               block off.  The default run's "the board did nothing" is only worth
               something if the same gesture does reach the board here - either the
               note moves (its box corner is inside its own interactive footprint)
               or the board gains an object (the wire the report was about). */
            report("text-box resize control: board-input block off moved=" +
                   std::to_string(stayedPut ? 0 : 1) + " components=" +
                   std::to_string(componentDelta) + " wires=" + std::to_string(wireDelta) +
                   " input-blocks=" + std::to_string(blockedInputs) + " resized=" +
                   std::to_string(resized ? 1 : 0));
            if (stayedPut && componentDelta == 0 && wireDelta == 0)
                report("text-box resize control FAILED: the board ignored the same drag");
            else
                report("PASS text-box resize control: without the block the same drag reached "
                       "the board");
        } else if (resized && stayedPut && textFollowsTop && blockedInputs > 0 &&
                   boardUntouched) {
            report("PASS text-box resize handles width=" +
                   std::to_string(resizeProbeWidth) + "->" +
                   std::to_string(config.widthUnits) + " height=" +
                   std::to_string(resizeProbeHeight) + "->" +
                   std::to_string(config.heightUnits) + " board-position=unchanged");
            report("PASS text-box text layout follows resized top edge");
            report("PASS text-box resize handles captured board input samples=" +
                   std::to_string(blockedInputs) + " board-objects=unchanged mouse-reads-hidden=" +
                   std::to_string(boardMouseReadsHidden));
        } else {
            report("text-box resize handles FAILED width=" +
                   std::to_string(resizeProbeWidth) + "->" +
                   std::to_string(config.widthUnits) + " height=" +
                   std::to_string(resizeProbeHeight) + "->" +
                   std::to_string(config.heightUnits) + " stayed=" +
                   std::to_string(stayedPut ? 1 : 0) + " text-at-top=" +
                   std::to_string(textFollowsTop ? 1 : 0) + " input-blocks=" +
                   std::to_string(blockedInputs) + " components=" +
                   std::to_string(componentDelta) + " wires=" + std::to_string(wireDelta) +
                   " counts-known=" + std::to_string(countsKnown ? 1 : 0) +
                   " samples=" + std::to_string(gestureSamples) + " leaked=" +
                   std::to_string(gestureLeaks) + " leaked-during-drag=" +
                   std::to_string(boardSampleUnblockedInGesture));
        }
        /* The synthetic test starts another drag immediately.  A real mouse
           release leaves at least one ordinary frame for the interaction loop
           to clear this; make the test boundary explicit so the next gesture
           cannot be mistaken for a continuation of the resize. */
        resizing = {};
        if (clearSelections) clearSelections();
        autotestStage = 85;
        autotestStageStart = now;
        return;
    }
    /* Prove the usable selection surface grew with the box: click near the
       resized right edge, beyond the type's fixed ±4-cell footprint. */
    if (autotestStage == 85 && now - autotestStageStart > 300) {
        const auto target = std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& note) {
            return note.instance == autotestInstance;
        });
        if (target == notes.end()) return;
        const Vec2 centre = toScreen(map, static_cast<float>(target->x),
                                     static_cast<float>(target->y));
        const NoteBox box = layoutNote(configFor(autotestInstance), map.unit, centre);
        selectionProbePoint = {box.maximum.x - map.unit * 0.35f,
                               (box.minimum.y + box.maximum.y) * 0.5f};
        hoverMouse(selectionProbePoint);
        autotestStage = 86;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 86 && now - autotestStageStart > 250) {
        clickAtScreenPoint(selectionProbePoint);
        autotestStage = 87;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 87 && now - autotestStageStart > 250) {
        releaseClickAtScreenPoint(selectionProbePoint);
        autotestStage = 88;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 88 && now - autotestStageStart > 500) {
        const auto target = std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& note) {
            return note.instance == autotestInstance;
        });
        const bool selected = target != notes.end() && boardSelection.valid() &&
                              boardSelection.isComponentSelected(target->index) != 0;
        const float fixedRight = target == notes.end()
                                     ? selectionProbePoint.x
                                     : toScreen(map, static_cast<float>(target->x),
                                                static_cast<float>(target->y)).x +
                                           kFootprintHalfWidth * map.unit;
        const float outside = (selectionProbePoint.x - fixedRight) /
                              std::max(1.f, map.unit);
        if (selected && outside > 0.2f) {
            report("PASS text-box native instance footprint selected outside type footprint by=" +
                   std::to_string(outside) + " cells");
        } else {
            report("text-box native instance footprint FAILED selected=" +
                   std::to_string(selected ? 1 : 0) + " outside=" +
                   std::to_string(outside));
        }
        if (clearSelections) clearSelections();
        autotestStage = 9;
        autotestStageStart = now;
        return;
    }
    /* The game prints nothing for the hidden probe, so everything below is
       evidence that the hit area the game still uses is the declared footprint
       and not the picture: the press lands on empty pixels three board units
       away from the centre, and the component has to move by one cell. */
    if (autotestStage == 9 && now - autotestStageStart > 500) {
        if (!defaultDrawingProbe || !dragProbe) {
            autotestStage = 13;
            return;
        }
        /* Close the editor first and press on a later frame: a press posted in
           the same frames the editor window is closing gets swallowed and the
           drag then reads "no movement" (measured once in a keep-selection run). */
        if (editing) {
            endEditing();
            autotestStageStart = now;
            return;
        }
        std::vector<NoteOnBoard> hidden;
        if (!collectInstancesOf(kHiddenProbeId, hidden)) return;
        const auto target = std::find_if(
            hidden.begin(), hidden.end(), [](const NoteOnBoard& note) {
                return note.x == kDragProbeX && note.y == kDragProbeY;
            });
        if (target == hidden.end()) return;
        hiddenDragInstance = target->instance;
        hiddenDragStart = *target;
        const Vec2 centre = toScreen(map, static_cast<float>(target->x),
                                     static_cast<float>(target->y));
        hiddenDragGrab = {centre.x + map.unit * 3.f, centre.y};
        if (!boardDragMouse(hiddenDragGrab, 0)) return;
        report("text-box autotest: pressed the hidden note at " +
               std::to_string(target->x) + "," + std::to_string(target->y) +
               " where the game drew nothing");
        autotestStage = 10;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 10 && now - autotestStageStart > 300) {
        boardDragMouse({hiddenDragGrab.x + map.unit, hiddenDragGrab.y}, 1);
        autotestStage = 11;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 11 && now - autotestStageStart > 300) {
        boardDragMouse({hiddenDragGrab.x + map.unit, hiddenDragGrab.y}, 2);
        autotestStage = 12;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 12 && now - autotestStageStart > 600) {
        std::vector<NoteOnBoard> hidden;
        bool found = false;
        int32_t toX = 0, toY = 0;
        if (collectInstancesOf(kHiddenProbeId, hidden)) {
            const auto after = std::find_if(
                hidden.begin(), hidden.end(), [](const NoteOnBoard& note) {
                    return note.instance == hiddenDragInstance;
                });
            if (after != hidden.end()) {
                found = true;
                toX = after->x;
                toY = after->y;
            }
        }
        TCBoardSnapshotV1 boardSnapshot{};
        boardSnapshot.size = sizeof(boardSnapshot);
        boardSnapshot.version = TC_BOARD_SNAPSHOT_VERSION_1;
        TCGameHandle board{};
        uint64_t selected = 0;
        if (boardApi.capture_snapshot && boardApi.get_current &&
            boardApi.get_current(boardApi.context, &board) == TC_HANDLE_OK &&
            boardApi.capture_snapshot(boardApi.context, &board, &boardSnapshot,
                                      static_cast<uint32_t>(sizeof(boardSnapshot))) ==
                TC_SNAPSHOT_OK)
            selected = boardSnapshot.selected_component_count;
        report("text-box autotest: hidden note drag from " +
               std::to_string(hiddenDragStart.x) + "," + std::to_string(hiddenDragStart.y) +
               " to " + std::to_string(toX) + "," + std::to_string(toY));
        if (found && toX == hiddenDragStart.x + 1 && toY == hiddenDragStart.y) {
            report("PASS text-box hidden note drag from " + std::to_string(hiddenDragStart.x) +
                   "," + std::to_string(hiddenDragStart.y) + " to " + std::to_string(toX) +
                   "," + std::to_string(toY) + " selection=" + std::to_string(selected) +
                   " instance=" + std::to_string(hiddenDragInstance));
        }
        autotestStage = 13;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 13 && now - autotestStageStart > 400) {
        if (keepSelection) {
            if (arcZoomOutNotches && !arcZoomOutDone) {
                arcZoomOutDone = true;
                zoomAtScreenPoint({1750.f, 800.f}, -arcZoomOutNotches);
                autotestStageStart = now;
                return;
            }
            /* The frame that gets captured is the one the gate measures, so the probe
               centres are re-reported here: a camera change after the first report
               would leave those pixel windows pointing at the old position. */
            if (defaultDrawingProbe) reportProbeCenters(map);
            /* The captured frame has to be the one where the suppressed note itself
               is still selected: the arc window is only meaningful if something is
               selected while it is measured.  The paired run with
               TC_TEXTBOX_KEEP_ARCS=1 is the sensitivity control for the very same
               window - it leaves the game's hint alone and reports hundreds of arc
               pixels there.  (Selecting a second, unsuppressed component in the same
               frame is not possible: a press on another component replaces the
               selection, and neither Shift-click nor the rubber band adds to it in
               this board state.) */
            report("text-box autotest: keeping the selection for the hint measurement");
            autotestStage = 16;
            autotestStageStart = now;
            return;
        }
        if (clearSelections) clearSelections();
        /* A drag also leaves a highlight on the component the game last moved;
           one press on empty board is what clears it. */
        boardDragMouse({2380.f, 1460.f}, 0);
        report("text-box autotest: cleared the game selection and pressed empty board");
        autotestStage = 14;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 14 && now - autotestStageStart > 250) {
        boardDragMouse({2380.f, 1460.f}, 2);
        hoverMouse({2380.f, 1460.f});
        autotestStage = 15;
        autotestStageStart = now;
        return;
    }
    if (autotestStage == 15 && now - autotestStageStart > 500) {
        TCBoardSnapshotV1 boardSnapshot{};
        boardSnapshot.size = sizeof(boardSnapshot);
        boardSnapshot.version = TC_BOARD_SNAPSHOT_VERSION_1;
        TCGameHandle board{};
        uint64_t selected = 0;
        if (boardApi.capture_snapshot && boardApi.get_current &&
            boardApi.get_current(boardApi.context, &board) == TC_HANDLE_OK &&
            boardApi.capture_snapshot(boardApi.context, &board, &boardSnapshot,
                                      static_cast<uint32_t>(sizeof(boardSnapshot))) ==
                TC_SNAPSHOT_OK)
            selected = boardSnapshot.selected_component_count;
        if (selected == 0)
            report("PASS text-box hidden note deselected selection=0 cleared=" +
                   std::to_string(clearSelections ? 1 : 0));
        else
            report("text-box autotest: the selection was still " + std::to_string(selected));
        autotestStage = 16;
        autotestStageStart = now;
    }
}

void frame(void*, const TCFrame*) {
    if (!host) return;
    /* The board sampled the previous frame's request before this callback.
       Rebuild the request from the handles currently under/holding the mouse. */
    const bool boardSampleBlockRequested = resizeBoardInputRequested;
    resizeBoardInputRequested = false;
    resizeGestureActive = false;
    resizePassClaimed = false;
    if (storeDirty && GetTickCount64() - storeSavedAt > 900) saveStore();
    refreshBoardKey();
    if (!boardUp()) {
        if (editing) endEditing();
        return;
    }
    /* Autotest watchdog: which frame changed the board's own object counts.  The
       node gesture must leave them alone, and when something does change them this
       line names the stage and whether the input block was in force at that
       moment - the difference between "the block leaks" and "another stage did it"
       is otherwise invisible in the log. */
    if (autotest) {
        uint64_t components = 0, wires = 0;
        static bool haveWatch = false;
        static uint64_t watchComponents = 0, watchWires = 0;
        if (boardObjectCounts(&components, &wires)) {
            if (!haveWatch) {
                haveWatch = true;
                watchComponents = components;
                watchWires = wires;
                report("text-box autotest: board objects at the first frame components=" +
                       std::to_string(components) + " wires=" + std::to_string(wires));
            } else if (components != watchComponents || wires != watchWires) {
                report("text-box autotest: board objects changed components=" +
                       std::to_string(watchComponents) + "->" + std::to_string(components) +
                       " wires=" + std::to_string(watchWires) + "->" + std::to_string(wires) +
                       " stage=" + std::to_string(autotestStage) + " block-requested=" +
                       std::to_string(boardSampleBlockRequested ? 1 : 0) + " resizing=" +
                       std::to_string(resizing.instance == autotestInstance ? 1 : 0));
                watchComponents = components;
                watchWires = wires;
                reportWireInventory();
            }
        }
    }
    const BoardMap map = boardMap();
    auto& api = tc::ui::drawing_detail::table();
    void* list = nullptr;
    if (map.valid && igGetBackgroundDrawList && igGetMainViewport)
        list = igGetBackgroundDrawList(igGetMainViewport());

    std::vector<NoteOnBoard> notes;
    liveNoteHalfExtents.clear();
    /* The enumeration needs the board, not the draw list.  Keeping the two apart
       matters: the mouse claim below has to be published even on a frame where
       there is nothing to draw with. */
    const bool notesKnown = collectNotes(notes);
    if (notesKnown) ++nodePassCount; else ++nodePassSkipped;
    /* Refresh the selection state the render callback reads.  The game's set is
       keyed by sequence index, so this maps every note to "selected". */
    selectedInstances.clear();
    if (boardSelection.valid()) {
        for (const NoteOnBoard& entry : notes)
            selectedInstances[entry.instance] =
                boardSelection.isComponentSelected(entry.index) != 0;
        /* The probe types draw through the same callback, so their instances
           have to be in the map as well. */
        if (defaultDrawingProbe) {
            std::vector<NoteOnBoard> probes;
            for (const uint64_t id : {kHiddenProbeId, kVisibleProbeId})
                if (collectInstancesOf(id, probes))
                    for (const NoteOnBoard& entry : probes)
                        selectedInstances[entry.instance] =
                            boardSelection.isComponentSelected(entry.index) != 0;
        }
    }
    const Vec2 mouse = tc::ui::mousePos();
    /* The mouse is claimed here, before anything below can fail or be skipped. */
    for (const NoteOnBoard& entry : notes) {
        const auto selected = selectedInstances.find(entry.instance);
        const bool isSelected = selected != selectedInstances.end() && selected->second;
        const bool forceResizeHandles = autotest && entry.instance == autotestInstance &&
                                        autotestStage >= 80 && autotestStage <= 84;
        const bool resizeHandlesEnabled = !autotest || forceResizeHandles;
        NoteConfig& config = configFor(entry.instance);
        const Vec2 centre = toScreen(map, static_cast<float>(entry.x),
                                     static_cast<float>(entry.y));
        const NoteBox box = layoutNote(config, map.unit, centre);
        if (noteTakesMouse(entry, config, box, map.unit, mouse, resizeHandlesEnabled,
                           resizeHandlesEnabled && (isSelected || forceResizeHandles)))
            resizePassClaimed = true;
    }
    if (resizing.instance && !tc::ui::isMouseDown(tc::ui::Mouse_Left) &&
        std::find_if(notes.begin(), notes.end(), [](const NoteOnBoard& entry) {
            return entry.instance == resizing.instance;
        }) == notes.end()) {
        /* The note under the drag is gone (deleted mid-gesture, or the level was
           replaced): drop the gesture instead of holding the board's mouse
           forever. */
        resizing = {};
    }
    if (!notesKnown && resizing.instance) {
        /* The note was being resized when the board enumeration failed: keep the
           claim for this frame instead of handing the mouse back mid-gesture. */
        if (blockBoardInput) resizeBoardInputRequested = true;
        resizeGestureActive = true;
    }
    if (notesKnown && list && api.rectFilled) {
        /* TC_TEXTBOX_LAYOUT_TRACE=1 prints one note's layout in board cells once a
           second: "the whole note scales with the camera" then reads as numbers that
           have to stay the same at every zoom, instead of as an impression. */
        if (layoutTrace && !notes.empty()) {
            static unsigned long long lastLayoutReportAt = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastLayoutReportAt > 1000) {
                lastLayoutReportAt = now;
                const NoteOnBoard& entry = notes.front();
                const NoteConfig& config = configFor(entry.instance);
                const Vec2 centre = toScreen(map, static_cast<float>(entry.x),
                                             static_cast<float>(entry.y));
                const NoteBox box = layoutNote(config, map.unit, centre);
                const float unit = map.unit > 0.f ? map.unit : 1.f;
                char line[256];
                std::snprintf(line, sizeof(line),
                              "text-box layout: unit=%.3f box=%.2fx%.2f cells "
                              "button=%.2f cells text=%.2f cells padding=%.2f cells",
                              unit, (box.maximum.x - box.minimum.x) / unit,
                              (box.maximum.y - box.minimum.y) / unit,
                              (box.buttonMaximum.x - box.buttonMinimum.x) / unit,
                              box.textSize / unit, box.padding / unit);
                report(line);
            }
        }
        void* handleList =
            (igGetForegroundDrawList && igGetMainViewport)
                ? igGetForegroundDrawList(igGetMainViewport())
                : list;
        for (const NoteOnBoard& entry : notes) {
            NoteConfig& config = configFor(entry.instance);
            const Vec2 centre = toScreen(map, static_cast<float>(entry.x),
                                         static_cast<float>(entry.y));
            const NoteBox box = layoutNote(config, map.unit, centre);
            const float footprintHalfWidth =
                (box.maximum.x - box.minimum.x) / (2.f * std::max(0.001f, map.unit));
            const float footprintHalfHeight =
                (box.maximum.y - box.minimum.y) / (2.f * std::max(0.001f, map.unit));
            liveNoteHalfExtents[entry.instance] =
                {footprintHalfWidth, footprintHalfHeight};
            /* Service enumeration supplies a handle here, so ordinary frames
               update immediately.  The render callback above repeats the call
               with its frame-scoped handle, covering the direct-table fallback. */
            if (entry.handle.size >= sizeof(TCGameHandle) &&
                geometryApiV2.set_instance_footprint)
                tc::component_geometry::setInstanceFootprint(
                    geometryApiV2, entry.handle, footprintHalfWidth, footprintHalfHeight);
            const bool hovered = mouse.x >= box.minimum.x && mouse.x <= box.maximum.x &&
                                 mouse.y >= box.minimum.y && mouse.y <= box.maximum.y;
            drawNote(list, map, entry, config, hovered);
            const auto selected = selectedInstances.find(entry.instance);
            const bool isSelected = selected != selectedInstances.end() && selected->second;
            drawLiveSelectionRange(handleList, map, entry, box, isSelected);
            const bool forceResizeHandles = autotest && entry.instance == autotestInstance &&
                                            autotestStage >= 80 && autotestStage <= 84;
            const bool resizeHandlesEnabled = !autotest || forceResizeHandles;
            const bool resizeInput =
                drawResizeHandles(handleList, map, entry, box, mouse,
                                  resizeHandlesEnabled && (isSelected || forceResizeHandles));
            if (!autotest || !autotestInstance || entry.instance == autotestInstance) {
                lastButtonCentre = {(box.buttonMinimum.x + box.buttonMaximum.x) * 0.5f,
                                    (box.buttonMinimum.y + box.buttonMaximum.y) * 0.5f};
                haveLastButton = true;
            }
            if (!hovered || resizeInput) continue;
            const bool onButton = mouse.x >= box.buttonMinimum.x &&
                                  mouse.x <= box.buttonMaximum.x &&
                                  mouse.y >= box.buttonMinimum.y && mouse.y <= box.buttonMaximum.y;
            if (tc::ui::isMouseClicked(tc::ui::Mouse_Left)) {
                unsigned long long& previous = lastClick[entry.instance];
                const unsigned long long now = GetTickCount64();
                const bool doubleClick = previous != 0 && now - previous < 420;
                previous = now;
                if (onButton || doubleClick) beginEditing(entry.instance, box.maximum);
            }
        }
    }
    drawEditor();

    {
        static unsigned long long lastReport = 0;
        const unsigned long long now = GetTickCount64();
        if (now - lastReport > 2000) {
            lastReport = now;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "text-box scan: source=%d first=%d queryCount=%llu "
                          "capture=%d components=%llu custom=%llu matched=%llu notes=%llu unit=%.1f",
                          lastScan.source, lastScan.firstStatus,
                          static_cast<unsigned long long>(lastScan.countFromQuery),
                          lastScan.captureStatus,
                          static_cast<unsigned long long>(lastScan.components),
                          static_cast<unsigned long long>(lastScan.customKind),
                          static_cast<unsigned long long>(lastScan.matched),
                          static_cast<unsigned long long>(notes.size()), map.unit);
            report(line);
        }
    }
    if (!measuredOnce && map.valid) {
        measuredOnce = true;
        const float sample = measureLine("MMMM", 20.f);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "text-box: board unit %.1f px, note count %u, measured \"MMMM\"@20 = %.1f px, "
                      "outputs=%u",
                      map.unit, static_cast<unsigned>(notes.size()), sample, registeredOutputs);
        report(line);
    }
    if (autotest && map.valid) runAutotest(notes, map);
    /* Z-order probe: paint the last selection hint's box on the foreground draw
       list.  If the game's own arcs disappear under it, the foreground list is
       the layer a Mod has to use to cover them. */
    if (foregroundTest && haveHintBox && igGetForegroundDrawList && igGetMainViewport) {
        void* front = igGetForegroundDrawList(igGetMainViewport());
        auto& draw = tc::ui::drawing_detail::table();
        if (front && draw.rectFilled)
            draw.rectFilled(front, hintBoxMin, hintBoxMax, rgba(255, 0, 255, 200), 0.f, 0);
    }
}

/* The component has no logic at all: it exists so that the board keeps a place
   for the note and the loader keeps a configuration for it.  Its one output
   pin is never connected; it drives 0 so that wiring it by accident cannot
   inject anything meaningful into a circuit. */
void noteLogic(TCLogicIOV2* io) {
    if (!io || io->phase == TC_LOGIC_RESET) return;
    if (io->outputs && io->output_count) io->outputs[0] = 0;
}

/* Schema 2 adds an explicit minimum height for direct manipulation.  Existing
   notes keep every style/text field and start at the old 3.6-cell minimum. */
struct NoteConfigV1 {
    uint32_t version;
    float fontUnits;
    float widthUnits;
    float paddingUnits;
    uint32_t background;
    uint32_t foreground;
    uint32_t flags;
    uint32_t align;
    char text[480];
};

int migrateNoteConfig(void*, uint32_t fromSchema, const void* fromData,
                      uint32_t fromBytes, uint32_t toSchema, void* out,
                      uint32_t capacity) {
    if (fromSchema != 1 || !fromData || fromBytes != sizeof(NoteConfigV1) ||
        toSchema != kConfigSchema || !out || capacity != sizeof(NoteConfig))
        return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    const auto& old = *static_cast<const NoteConfigV1*>(fromData);
    NoteConfig migrated = defaultConfig();
    migrated.fontUnits = old.fontUnits;
    migrated.widthUnits = old.widthUnits;
    migrated.paddingUnits = old.paddingUnits;
    migrated.background = old.background;
    migrated.foreground = old.foreground;
    migrated.flags = old.flags;
    migrated.align = old.align;
    std::memcpy(migrated.text, old.text, sizeof(migrated.text));
    migrated.text[sizeof(migrated.text) - 1] = 0;
    std::memcpy(out, &migrated, sizeof(migrated));
    return TC_COMPONENT_CONFIG_MIGRATE_OK;
}

/* ---------------------------------------------------------------------------
   Loading
   ------------------------------------------------------------------------ */

template <class T>
T engine(const char* name) {
    T value = nullptr;
    void* found = host->engine_proc(host->context, name);
    static_assert(sizeof(value) == sizeof(found));
    std::memcpy(&value, &found, sizeof(value));
    return value;
}

template <class T>
T alias(const char* name) {
    T value = nullptr;
    void* found = host->resolve_alias(host->context, name);
    static_assert(sizeof(value) == sizeof(found));
    std::memcpy(&value, &found, sizeof(value));
    return value;
}

template <class T>
T symbol(const char* name) {
    T value = nullptr;
    void* found = host->resolve_symbol ? host->resolve_symbol(host->context, name) : nullptr;
    static_assert(sizeof(value) == sizeof(found));
    std::memcpy(&value, &found, sizeof(value));
    return value;
}

void resolveFonts() {
    void** table = alias<void**>("ui.fonts");
    if (!table) return;
    /* The table is null terminated (the loader walks it the same way); the
       entries are: [0] the icon font, [1] the bold face, [2] the regular one
       the board and the panels draw with. */
    for (int index = 0; index < 8; ++index) {
        void* font = table[index];
        if (!font) break;
        const char* name = imFontDebugName ? imFontDebugName(font) : nullptr;
        if (!name || !*name) continue;
        if (!boldFont && std::strstr(name, "Bold")) boldFont = font;
        if (!regularFont && std::strstr(name, "Regular")) regularFont = font;
    }
    if (!regularFont) regularFont = table[2] ? table[2] : table[0];
    if (!boldFont) boldFont = regularFont;
}

int registerNote(uint32_t outputs, uint64_t customId = kNoteId,
                 const char* typeId = "local.text-box/note",
                 const char* fixedName = nullptr) {
    static const TCComponentPinV2 outputPin{"out", "Out", 1, 0};
    const NoteConfig defaults = defaultConfig();
    /* Development knobs, used by tests/text-box-playtest.ps1 to measure how the
       game draws a Mod-registered custom component on the board (the name and
       the icon come from the prototype and are not part of the SDK).  They are
       inert unless the environment sets them. */
    static char nameOverride[64]{};
    static char svgOverride[256]{};
    const char* name = fixedName ? fixedName : "文本框";
    const char* shape = nullptr;
    if (!fixedName &&
        GetEnvironmentVariableA("TC_TEXTBOX_NAME", nameOverride, sizeof(nameOverride)) > 0 &&
        nameOverride[0])
        name = nameOverride;
    if (!fixedName &&
        GetEnvironmentVariableA("TC_TEXTBOX_SVG", svgOverride, sizeof(svgOverride)) > 0 &&
        svgOverride[0])
        shape = svgOverride;
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = customId;
    definition.type_id = typeId;
    definition.name = name;
    definition.shape_svg = shape;
    definition.description = "说明用的文本框：0 输入 0 输出，不参与电路。";
    definition.inputs = nullptr;
    definition.input_count = 0;
    definition.outputs = outputs ? &outputPin : nullptr;
    definition.output_count = outputs;
    definition.state_words = 0;
    definition.gate_cost = 0;
    definition.delay = 0;
    definition.callback = &noteLogic;
    definition.config_schema = kConfigSchema;
    definition.config_size = sizeof(NoteConfig);
    definition.default_config = &defaults;
    definition.config_migration_version = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
    definition.migrate_config = &migrateNoteConfig;
    return tc::component_types::registerDefinition(typesApi, &definition);
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) {
        report("text-box: tc::ui::load failed: " + tc::ui::missing());
        return 2;
    }
    if (!tc::ui::loadDrawing(h)) {
        report("text-box: drawing unavailable: " + tc::ui::drawingMissing());
        return 3;
    }
    if (!tc::hostHas(h, TC_CAP_SERVICES) || !tc::hostHas(h, TC_CAP_SYMBOL_ALIAS)) {
        report("text-box: this loader has no services or aliases");
        return 4;
    }
    if (tc::boardService(h, &boardApi) != TC_SERVICE_OK) {
        report("text-box: tc.board V6 is unavailable");
        return 5;
    }
    char defaultProbeSetting[8]{};
    defaultDrawingProbe =
        GetEnvironmentVariableA("TC_TEXTBOX_DEFAULT_PROBE", defaultProbeSetting,
                                sizeof(defaultProbeSetting)) > 0;
    char dragProbeSetting[8]{};
    if (GetEnvironmentVariableA("TC_TEXTBOX_DRAG_PROBE", dragProbeSetting,
                                sizeof(dragProbeSetting)) > 0 &&
        dragProbeSetting[0] == '0')
        dragProbe = false;
    char keepSelectionSetting[8]{};
    keepSelection =
        GetEnvironmentVariableA("TC_TEXTBOX_KEEP_SELECTION", keepSelectionSetting,
                                sizeof(keepSelectionSetting)) > 0 &&
        keepSelectionSetting[0] != '0';
    char foregroundSetting[8]{};
    foregroundTest =
        GetEnvironmentVariableA("TC_TEXTBOX_FOREGROUND_TEST", foregroundSetting,
                                sizeof(foregroundSetting)) > 0 &&
        foregroundSetting[0] != '0';
    char arcsSetting[8]{};
    keepGameArcs = GetEnvironmentVariableA("TC_TEXTBOX_KEEP_ARCS", arcsSetting,
                                           sizeof(arcsSetting)) > 0 &&
                   arcsSetting[0] != '0';
    char hintColourSetting[8]{};
    hintOrange = GetEnvironmentVariableA("TC_TEXTBOX_HINT_ORANGE", hintColourSetting,
                                         sizeof(hintColourSetting)) > 0 &&
                 hintColourSetting[0] != '0';
    char arcZoomSetting[8]{};
    if (GetEnvironmentVariableA("TC_TEXTBOX_ARC_ZOOM_OUT", arcZoomSetting,
                               sizeof(arcZoomSetting)) > 0)
        arcZoomOutNotches = std::atoi(arcZoomSetting);
    char layoutTraceSetting[8]{};
    layoutTrace = GetEnvironmentVariableA("TC_TEXTBOX_LAYOUT_TRACE", layoutTraceSetting,
                                          sizeof(layoutTraceSetting)) > 0 &&
                  layoutTraceSetting[0] != '0';
    char foundrySetting[8]{};
    keepFoundryButton = GetEnvironmentVariableA("TC_TEXTBOX_FOUNDRY_BUTTON", foundrySetting,
                                                sizeof(foundrySetting)) > 0 &&
                        foundrySetting[0] != '0';
    char blockInputSetting[8]{};
    if (GetEnvironmentVariableA("TC_TEXTBOX_BLOCK_BOARD_INPUT", blockInputSetting,
                                sizeof(blockInputSetting)) > 0 &&
        blockInputSetting[0] == '0')
        blockBoardInput = false;
    if (tc::component_types::table(h, &typesApi)) {
        /* The note is a true 0-in/0-out component: it owns a place on the board
           and a per-instance configuration, and it never observes or produces a
           value.  The loader writes the single unconnected driver a sink gets,
           so a definition with no pin at all is accepted and the board record
           still carries the instance id this Mod keys its text on
           (docs/research/text-component.md §1).
           TC_TEXTBOX_OUTPUT_PIN=1 registers the older shape - one dangling
           output at (+2,0) - which is kept so the two can be compared in the
           sandbox without a rebuild. */
        char shapeSetting[8]{};
        const bool danglingOutputPin =
            GetEnvironmentVariableA("TC_TEXTBOX_OUTPUT_PIN", shapeSetting,
                                    sizeof(shapeSetting)) > 0 &&
            shapeSetting[0] != '0';
        const uint32_t declaredOutputs = danglingOutputPin ? 1u : 0u;
        const int status = registerNote(declaredOutputs);
        if (status != TC_COMPONENT_TYPES_OK) {
            report("text-box: the component could not be registered: " +
                   std::string(tc::component_types::errorText(status)));
            return 6;
        }
        registeredOutputs = declaredOutputs;
        if (defaultDrawingProbe) {
            const int hidden = registerNote(declaredOutputs, kHiddenProbeId,
                                            "local.text-box/default-hidden-probe",
                                            "DEFAULT HIDDEN");
            const int visible = registerNote(declaredOutputs, kVisibleProbeId,
                                             "local.text-box/default-visible-probe",
                                             "DEFAULT VISIBLE");
            if (hidden != TC_COMPONENT_TYPES_OK || visible != TC_COMPONENT_TYPES_OK) {
                report("text-box: default drawing probe types could not be registered");
                return 6;
            }
        }
    } else {
        report("text-box: tc.component.types is unavailable");
        return 7;
    }
    /* The note now reserves the same fixed board rectangle the player sees.
       Footprint is also the game's pointer drag area, so empty corners of the
       note remain selectable even though the built-in custom thumbnail is much
       smaller.  The service rounds outward to the integer grid: 4x2 half
       extents means an 8x4 board-space box. */
    if (!tc::component_geometry::table(h, &geometryApi)) {
        report("text-box: tc.component.geometry is unavailable");
        return 8;
    }
    if (!tc::component_geometry::tableV2(h, &geometryApiV2)) {
        report("text-box: tc.component.geometry V2 is unavailable");
        return 8;
    }
    const int geometryStatus =
        tc::component_geometry::setFootprint(geometryApi, kNoteId, kFootprintHalfWidth,
                                             kFootprintHalfHeight);
    if (geometryStatus != TC_COMPONENT_GEOMETRY_OK) {
        report("text-box: footprint registration failed: " +
               std::string(tc::component_geometry::errorText(geometryStatus)));
        return 9;
    }
    report("text-box: type footprint registered half=4.0,2.0; live instances follow rendered box size");
    if (defaultDrawingProbe) {
        /* The hidden probe repeats the note's 8x4 box so the default-drawing
           case can press a spot the game never printed on and still move the
           component. */
        const int probeFootprint =
            tc::component_geometry::setFootprint(geometryApi, kHiddenProbeId, kFootprintHalfWidth,
                                                 kFootprintHalfHeight);
        if (probeFootprint != TC_COMPONENT_GEOMETRY_OK) {
            report("text-box: probe footprint registration failed: " +
                   std::string(tc::component_geometry::errorText(probeFootprint)));
            return 9;
        }
        /* The visible control is only about the game's own drawing, but it
           carries the same box so its selection hint is truthful too. */
        const int visibleFootprint =
            tc::component_geometry::setFootprint(geometryApi, kVisibleProbeId, kFootprintHalfWidth,
                                                 kFootprintHalfHeight);
        if (visibleFootprint != TC_COMPONENT_GEOMETRY_OK) {
            report("text-box: probe footprint registration failed: " +
                   std::string(tc::component_geometry::errorText(visibleFootprint)));
            return 9;
        }
    }
    if (!tc::component_render::tableV2(h, &renderApi)) {
        report("text-box: tc.component.render V2 is unavailable");
        return 10;
    }
    /* V3: the note draws its own footprint-sized selection hint, so the game's
       own arcs (which follow the component mesh, not the footprint) are turned
       off for this type.  The visible probe keeps them as the control. */
    tc::component_render::ApiV3 renderApiV3{};
    const bool haveSelectionHintControl = tc::component_render::tableV3(h, &renderApiV3);
    if (haveSelectionHintControl && !keepGameArcs) {
        const int hintStatus =
            tc::component_render::setSelectionHint(renderApiV3, kNoteId, false);
        if (hintStatus != TC_COMPONENT_RENDER_OK) {
            report("text-box: disabling the game's selection hint failed: " +
                   std::string(tc::component_render::errorText(hintStatus)));
            return 11;
        }
        if (defaultDrawingProbe) {
            const int probeHint =
                tc::component_render::setSelectionHint(renderApiV3, kHiddenProbeId, false);
            if (probeHint != TC_COMPONENT_RENDER_OK) {
                report("text-box: disabling the probe selection hint failed: " +
                       std::string(tc::component_render::errorText(probeHint)));
                return 11;
            }
        }
        report("text-box: the game's own selection arcs are off for the note type");
    } else {
        report("text-box: tc.component.render V3 is unavailable; the game's selection arcs stay on");
    }
    /* V4: this Mod's types have no player-editable schematic (the logic comes from
       the Mod, the shape from the declared footprint), so the game's "edit this
       component in the foundry" button on the component panel cannot do anything
       useful with them and is dropped.  TC_TEXTBOX_FOUNDRY_BUTTON=1 keeps it as the
       control for the pixel measurement. */
    tc::component_render::ApiV4 renderApiV4{};
    const bool haveFoundryButtonControl = tc::component_render::tableV4(h, &renderApiV4);
    if (!haveFoundryButtonControl) {
        report("text-box: tc.component.render V4 is unavailable; the foundry edit button stays");
    } else if (keepFoundryButton) {
        report("text-box: keeping the game's foundry edit button as the control "
               "(TC_TEXTBOX_FOUNDRY_BUTTON=1)");
    } else {
        const int foundryStatus =
            tc::component_render::setFoundryButton(renderApiV4, kNoteId, false);
        if (foundryStatus != TC_COMPONENT_RENDER_OK) {
            report("text-box: dropping the foundry edit button failed: " +
                   std::string(tc::component_render::errorText(foundryStatus)));
            return 12;
        }
        if (defaultDrawingProbe) {
            for (const uint64_t id : {kHiddenProbeId, kVisibleProbeId}) {
                const int probeFoundry =
                    tc::component_render::setFoundryButton(renderApiV4, id, false);
                if (probeFoundry != TC_COMPONENT_RENDER_OK) {
                    report("text-box: dropping the probe foundry edit button failed: " +
                           std::string(tc::component_render::errorText(probeFoundry)));
                    return 12;
                }
            }
        }
        report("text-box: the game's foundry edit button is off for the note type");
    }
    const int defaultStatus =
        tc::component_render::setDefaultDrawing(renderApi, kNoteId, false);
    if (defaultStatus != TC_COMPONENT_RENDER_OK) {
        report("text-box: disabling default drawing failed: " +
               std::string(tc::component_render::errorText(defaultStatus)));
        return 11;
    }
    if (defaultDrawingProbe) {
        const int probeDefaultStatus =
            tc::component_render::setDefaultDrawing(renderApi, kHiddenProbeId, false);
        if (probeDefaultStatus != TC_COMPONENT_RENDER_OK) {
            report("text-box: disabling probe default drawing failed: " +
                   std::string(tc::component_render::errorText(probeDefaultStatus)));
            return 11;
        }
    }
    report("text-box: game default drawing disabled for note type");
    const int renderStatus =
        tc::component_render::setDrawCallback(renderApi, kNoteId, &renderNoteOutline);
    if (renderStatus != TC_COMPONENT_RENDER_OK) {
        report("text-box: render callback registration failed: " +
               std::string(tc::component_render::errorText(renderStatus)));
        return 11;
    }
    if (defaultDrawingProbe) {
        /* The probe types take part in the selection-hint measurement: the drag
           case selects one of them and the captured frame has to show the hint
           at the declared footprint. */
        for (const uint64_t id : {kHiddenProbeId, kVisibleProbeId}) {
            const int probeRender =
                tc::component_render::setDrawCallback(renderApi, id, &renderNoteOutline);
            if (probeRender != TC_COMPONENT_RENDER_OK) {
                report("text-box: probe render callback registration failed: " +
                       std::string(tc::component_render::errorText(probeRender)));
                return 11;
            }
        }
    }
    worldToScreen = alias<V2 (*)(V2)>("board.world_to_screen");
    if (!boardSelection.load(h))
        report("text-box: the game's selection set is unavailable; the note draws no selection hint");
    /* The game's own deselect entry: the drag case uses it after it moved the
       hidden note, so the captured frame is free of a selection highlight. */
    clearSelections = symbol<void (*)()>("clear_selections__modelZboardZboard_u8323");
    if (!clearSelections)
        report("text-box: the game's clear-selection entry was not found");
    void* boardInputSample = h->resolve_symbol(h->context, "igIsAnyItemActive");
    if (!boardInputSample || !h->create_hook ||
        h->create_hook(h->context, boardInputSample,
                       reinterpret_cast<void*>(&hookIsAnyItemActive),
                       reinterpret_cast<void**>(&igIsAnyItemActiveOriginal)) != 0) {
        report("text-box: resize input capture hook is unavailable");
        return 12;
    }
    /* The hook itself stays installed in both modes, so the control run isolates
       the request flag rather than the hook. */
    report(blockBoardInput
               ? "text-box: resize handles own the board mouse while hovered or dragged"
               : "text-box: resize input block is OFF (control run)");
    /* The mouse-state hooks that actually keep the press away from the board.  A
       missing one is reported rather than fatal: the Mod still works, it just
       leaves the board's own reading of the gesture in place. */
    {
        struct MouseHook {
            const char* symbol;
            void* detour;
            void** original;
        };
        const MouseHook mouseHooks[] = {
            {"igIsMouseDown_ID", reinterpret_cast<void*>(&hookIsMouseDownId),
             reinterpret_cast<void**>(&igIsMouseDownIdOriginal)},
            {"igIsMouseClicked_InputFlags", reinterpret_cast<void*>(&hookIsMouseClicked),
             reinterpret_cast<void**>(&igIsMouseClickedOriginal)},
            {"igIsMouseDoubleClicked_ID", reinterpret_cast<void*>(&hookIsMouseDoubleClicked),
             reinterpret_cast<void**>(&igIsMouseDoubleClickedOriginal)},
            {"igIsMouseReleased_ID", reinterpret_cast<void*>(&hookIsMouseReleased),
             reinterpret_cast<void**>(&igIsMouseReleasedOriginal)},
        };
        int armed = 0;
        for (const MouseHook& entry : mouseHooks) {
            void* target = h->resolve_symbol ? h->resolve_symbol(h->context, entry.symbol)
                                             : nullptr;
            if (!target || !h->create_hook ||
                h->create_hook(h->context, target, entry.detour, entry.original) != 0)
                continue;
            ++armed;
        }
        report("text-box: board mouse-state hooks armed=" + std::to_string(armed) + "/4");
    }
    loadedLevelName = h->resolve_alias(h->context, "level.loaded");
    schematicPathSlot = h->resolve_alias(h->context, "save.path.schematic");
    emutlsAddress = alias<void* (*)(void*)>("runtime.emutls");
    igGetIO = engine<void* (*)()>("igGetIO");
    igGetMainViewport = engine<void* (*)()>("igGetMainViewport");
    igGetBackgroundDrawList = engine<void* (*)(void*)>("igGetBackgroundDrawList");
    imFontDebugName = engine<const char* (*)(const void*)>("ImFont_GetDebugName");
    fontCalcTextSize = engine<void (*)(V2*, const void*, float, float, float, const char*,
                                      const char*, const char**)>("ImFont_CalcTextSizeA");
    igColorEdit4 = engine<bool (*)(const char*, float*, int)>("igColorEdit4");
    igInputTextMultiline =
        engine<bool (*)(const char*, char*, unsigned long long, V2, int, void*, void*)>(
            "igInputTextMultiline");
    igSetNextFrameWantCaptureMouse =
        engine<void (*)(bool)>("igSetNextFrameWantCaptureMouse");
    tc::commandService(h, &commandApi);
    resolveFonts();
    if (!worldToScreen || !igGetIO || !igGetMainViewport || !igGetBackgroundDrawList)
        report("text-box: the board transform is unavailable; notes stay invisible");
    if (!regularFont) report("text-box: the game's text font could not be resolved");
    igGetForegroundDrawList = engine<void* (*)(void*)>("igGetForegroundDrawList");

    if (h->data_directory_utf8 && *h->data_directory_utf8) {
        storePath = std::string(h->data_directory_utf8) + "\\notes.txt";
        loadStore();
        std::string probe = std::string(h->data_directory_utf8) + "\\autotest.txt";
        std::ifstream file(probe, std::ios::binary);
        if (file) {
            std::string level;
            std::getline(file, level);
            autotest = true;
            report("text-box autotest: armed" + (level.empty() ? std::string() : " for " + level));
        }
    }

    out->on_frame = &frame;
    note(std::string("文本框元件：") +
             (registeredOutputs == 0 ? "0 输入 0 输出" : "0 输入 1 输出（输出悬空，不参与电路）") +
             "；在元件列表里放置，点右上角编辑按钮输入文字",
         0);
    return 0;
}
