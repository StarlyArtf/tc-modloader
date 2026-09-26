/* State and helpers the two M2 implementation files share:
   components.cpp owns configuration, logic, drawing and registration,
   components_ui.cpp owns hover, clicks and the two editor windows. */

#ifndef float_ops_components_internal_hpp
#define float_ops_components_internal_hpp

#include "../sdk/tc_component_instances.h"
#include "../sdk/tc_component_geometry.h"
#include "../sdk/tc_component_render.h"
#include "../sdk/tc_component_storage.h"
#include "../sdk/tc_simulation.h"
#include "../sdk/tc_ui.h"
#include "components.hpp"
#include "fp/decimal.hpp"
#include "fp/fp32.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace floatops {
namespace internals {

extern const TCHost* host;
extern tc::component_types::Api types;
extern tc::component_render::ApiV2 render;
extern tc::component_render::ApiV6 renderV6;
extern tc::component_geometry::Api geometry;
extern tc::component_storage::ApiV2 storage;
/* The same table seen through the V1 type: read/write configuration only, which
   is the part both versions share (V2's layout begins with V1's fields). */
extern tc::component_storage::Api storageV1;
extern tc::component_instances::Api instances;
extern bool uiReady;
extern bool renderReady;
extern bool placementPreviewReady;

struct MousePoint {
    float x, y;
};
extern void (*getMousePos)(MousePoint*);
extern bool (*mouseClicked)(int, bool);
extern bool (*mouseReleased)(int);

/* Layout, in board cells, taken from the original word parts (plan 7.2 and
   docs/research/component-appearance.md).

   The stock Constant/Static Value picture is 4.92 x 2.93 cells with its label
   in the top right corner, its width box in the top left and its value in the
   middle; both kinds list `out0=(3,0)` in the kind table, so their pins sit on
   the 3.0 lane *outside* that body.  This Mod therefore declares
   `pin_lane = 3.0` for its types (sdk/tc_service_api.h,
   TCComponentTypeDefinitionV2::pin_lane): at the loader's default 2.0 lane the
   pin lands inside a stock-sized body, which is what the board used to show.
   The generated wires follow the pins, so the type still compiles and runs.

   Board cells here run with +y *down* the screen - the loader's own frame
   convention, measured: the upper input of a two-pin side is row 0 and the
   lower one row +1 (`in0=(-2,0) in1=(-2,1)`).  "Top" therefore means the
   negative y edge. */
constexpr float kFootprintHalfWidth = 2.5f;
constexpr float kFootprintHalfHeight = 1.5f;
constexpr float kBodyHalfWidth = 2.46f;   /* the stock part is 4.92 cells wide */
constexpr float kBodyHalfHeight = 1.465f; /* ... and 2.93 cells tall */
/* The pins sit on the lane the type declared, one row apart, exactly like the
   game lays a two-pin side out (in0=(-3,0) in1=(-3,1) at this lane). */
constexpr float kPinLaneX = 3.0f;
constexpr float kPinRowFirst = 0.f;
constexpr float kPinRowSecond = 1.f;
/* The stock part's pin is a disc about 0.33 cells across (10 px radius at the
   30.3 px/cell the reference board was captured at). */
constexpr float kPinRadius = 0.33f;

/* The width box in the top left and the label in the top right, inset by the
   same 0.17 cells, which is where the stock picture puts them (measured on the
   board: the box covers 0.17..1.16 cells from the left edge, 0.165 cells below
   the top edge). */
constexpr float kBoxInset = 0.17f;
constexpr float kBoxWidth = 1.00f;
constexpr float kBoxHeight = 0.66f;
constexpr float kLabelInset = 0.21f; /* the label's right edge, from the body's */

/* Text sizes as digit heights in cells, again from the stock parts: the value
   is 0.59 cells tall (18 px on the reference capture, at 30.3 px per cell) and
   the label and width digits 0.40 (11 px and 12 px).

   Turning a digit height into the size AddText wants takes two measured
   numbers, both from the same reference capture cross-checked with the face's
   own metrics: the game paints a face at 0.79 of the size it is given (asking
   for 21.07 px produced a 16.7 px advance, 14.29 px produced 11.3 px), and a
   painted digit is 0.66 of that painted size high.  Their product is the one
   number the layout uses: a digit is 0.52 * size tall and its ink centre sits
   0.52 * size below the origin AddText takes, so a component can be laid out in
   digit heights alone. */
constexpr float kValueCapCells = 0.59f;
constexpr float kLabelCapCells = 0.40f;
constexpr float kWidthCapCells = 0.40f;
constexpr float kPaintedPerAsked = 0.79f;
constexpr float kCapPerPainted = 0.66f;
constexpr float kCapPerAsked = kPaintedPerAsked * kCapPerPainted;
/* A value wider than the body is shrunk until it fits this many cells. */
constexpr float kValueMaxCells = 4.10f;
/* The two-line display: the value row's centre, then the bit pattern's. */
constexpr float kDisplayValueRow = -0.30f;
constexpr float kDisplayHexRow = 0.62f;
constexpr float kDisplayHexCapCells = 0.40f;

struct ScreenBox {
    float min_x, min_y, max_x, max_y;
};

struct HitBox {
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool valid = false;
    bool hovered = false;
};

/* Everything the last painted frame left on screen for one instance. */
struct InstanceBoxes {
    HitBox body;
    HitBox width;
    HitBox rounding;
    uint64_t custom_id = 0;
    int frame = -1;
};

extern std::mutex hitMutex;
extern std::map<uint64_t, InstanceBoxes> boxes;
extern int frameCounter;

/* The Display's last input and its formatted text: written by the logic
   callback, read by the render callback and by the true-game test. */
extern std::mutex valueMutex;
extern std::map<uint64_t, uint32_t> values;
extern std::map<uint64_t, std::string> texts;

/* The drawer's editor rows: what a row edits, and the text a player is typing
   (ImGui keeps the rest of the field state itself). */
/* `info` is a row with no widget: the catalogue types that have no rounding
   choice show what their pins mean instead (bit order of Classify, the
   saturating policy of the conversions, the pin split of Split/Make Bits). */
enum class RowKind { none, label, value, rounding, display, info };

/* One painted row: where it is on screen and what it edits.  The rows are real
   widgets (see the row window in components_ui.cpp), so `live` is what ImGui
   answered about the row's own field - the playtest reads it, and a row that a
   build does not deliver input to is still reported as such. */
struct PanelRow {
    HitBox box;
    RowKind kind = RowKind::none;
    bool live = false;
};

/* The configuration the rows were filled from, byte for byte.

   The instance id alone cannot answer "do these rows still describe this
   component's configuration?": a level that is left and entered again reuses the
   board's component ids, the loader restores a saved record on the first frame
   that can see it (which can be later than the drawer's first read), and an undo
   or a clone moves the same bytes again.  Keeping the bytes the rows were built
   from turns all of those into one question - "does the record still hold what
   the fields show?" - so the player never has to click a field to make the panel
   agree with the board. */
constexpr uint32_t kConfigSnapshotBytes = 32;

struct ConfigSnapshot {
    uint64_t type = 0;      /* the custom id the bytes belong to, 0 = none */
    uint32_t size = 0;
    bool valid = false;     /* the record could be read at all */
    uint8_t bytes[kConfigSnapshotBytes] = {};
};

struct EditorState {
    uint64_t instance = 0;      /* the instance the rows are showing, 0 = none */
    char label[64] = {};        /* the label field */
    char value[64] = {};        /* the value field (Constant only) */
    std::string message;        /* why the last commit was refused, if it was */
    PanelRow rows[4];           /* the panel rows of the last painted frame */
    int rowCount = 0;
    ConfigSnapshot loaded;      /* what the record held when the rows were filled */
};
extern EditorState editor;

/* Drawing helpers (components.cpp). */
ScreenBox screenBox(const TCComponentRenderFrameV1& frame, float x0, float y0, float x1,
                    float y1);
HitBox hitBox(const TCComponentRenderFrameV1& frame, float x0, float y0, float x1,
              float y1);

/* Interaction helpers (components_ui.cpp). */
/* Registers the editor rows that live in the game's own component drawer (the
   panel along the bottom for the selected component) and keeps the hover state
   fresh once per frame. */
void registerEditors(const TCHost* host);
void frameCallback(void* user, const TCFrame* frame);

/* Configuration access (components.cpp). */
bool writeConfig(uint64_t instance, const void* data, uint32_t bytes);
bool currentConfig(uint64_t instance, uint64_t custom_id, void* out, uint32_t bytes);
bool boxesOf(uint64_t instance, InstanceBoxes* out);
/* Which registered type a live instance is, asked of the host instead of the
   last painted frame: a board that was just entered has no boxes yet. */
uint64_t typeOfLiveInstance(uint64_t instance);
/* A configuration edit and a freshly bound Constant both need one paused-board
   evaluation.  The request is serviced from the next frame, after the host has
   finished the edit/compile that caused it. */
void servicePendingBoardRefresh();
/* Successful configuration commits are written into the component's own tail
   record, then the next frame asks the game to save that circuit. */
void servicePendingCircuitSave();
/* Watches the Board handle the editor rows belong to.  A new board (a re-entered
   level, which reuses its component ids) drops the rows and everything cached for
   the board that is gone, so the next read comes from the new records. */
void serviceBoardChange();

/* Defaults, names and logging also live in components.cpp. */
void note(const std::string& text);
ConstantConfig constantDefault();
AddConfig addDefault();
DisplayConfig displayDefault();
OpsConfig opsDefault();
tcfp::FPRounding roundingOf(uint8_t code);
tcfp::DisplayMode displayModeOf(uint8_t code);
const char* roundingCode(tcfp::FPRounding rounding);
const char* roundingName(tcfp::FPRounding rounding);
const char* displayModeName(tcfp::DisplayMode mode);

}  // namespace internals
}  // namespace floatops

#endif
