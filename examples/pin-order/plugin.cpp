/* Pin order: the game's left IO panel lists the pins of the board but never
   lets the player change that order.  This Mod draws a thin frame around every
   entry with a grip strip on its left edge and drags the entry by that strip:
   the row the pointer is dropped on takes the dragged pin's place, and
   everything between the two shifts by one.  Ctrl-clicking a grip puts the
   group back into the panel's own order.

   The order itself belongs to the loader (TC_SERVICE_PIN_ORDER): the panel
   draws three cached sequences, the order of those elements *is* the order the
   player sees, and the service permutes them.  Nothing here writes game memory.

   Geometry.  The panel positions every entry with igSetCursorPos, and the call
   sites inside build_io_state_view say which part of an entry is being placed:

       0x6d4, 0x1e6c   an input's label line (two sites, the same line)
       0x1836, 0x189d  an output's label line
        0x21a9, 0x28c8, 0x295d, 0x3257
                        the multi-bit squares under a label, one call per square

   So an entry is a label line and, one line below it, the squares; a pin wider
   than one bit also shows its value under those squares.  The frame starts half
   a line above the label, but its lower edge uses the bit control's actual ImGui
   item height (the one-bit control is deliberately much larger than half a
   line), plus the value line where present.  Multi-bit controls are measured at
   their cursor-reset calls; the separate 0/1-bit branch is measured at its
   per-entry scalar end anchor.  The anchors only become complete at the
   end of an entry, so the handles are drawn at the start of the next frame,
   from the entries that frame's panel laid out: one frame of lag at 60 fps, and
   the frame is behind the content instead of over it.

   Drawing is a draw-list overlay, never widgets: an invisible button over an
   entry would swallow the clicks and the typing the game's own value field
   needs.  The clip is the entry, the grip strip is the only hit area, and the
   layout cursor is left exactly where the game put it. */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_pin_order.h"
#include "../../sdk/tc_hook.h"
#include "layout.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const TCHost* host;

/* The panel: symbol and size, and the call sites above, all measured on this
   build (tests/pin-order-probe.cpp prints the same anchors). */
static uint8_t* ioStateView;
static constexpr uintptr_t kPanelSize = 0x3fc0;
static constexpr uintptr_t kInputLabelSites[] = {0x6d4, 0x1e6c};
static constexpr uintptr_t kOutputLabelSites[] = {0x1836, 0x189d};
static constexpr uintptr_t kSquareSites[] = {0x21a9, 0x28c8, 0x295d, 0x3257};

static constexpr float kFrameLeft = 6.f;
static constexpr float kFrameInset = 12.f;
/* The two little buttons that move an entry one slot up or down.  They sit at
   the entry's left edge, straddling its label line, so they stay on screen
   whatever an entry's height turns out to be. */
static constexpr float kButtonLeft = 3.f;
static constexpr float kButtonSize = 15.f;
static constexpr float kButtonGap = 2.f;
static constexpr float kRhythmPerWidth = 0.36f;
static constexpr float kAbovePerRhythm = 0.10f;
static constexpr float kFallbackBelowPerRhythm = 0.62f;
/* Where the second line (the squares) sits below the label when the panel does
   not say: 35 px of a 120 px rhythm on this build. */
static constexpr float kLinePerRhythm = 0.29f;
/* How much of a rhythm the panel leaves between one entry's content and the
   next entry's line, and the point past which an entry is taller than the one
   the panel sized its rhythm for. */
static constexpr float kBandGapPerRhythm = 0.24f;
static constexpr float kGrownBand = 1.2f;
static constexpr float kReportedBoundsPadding = 4.f;

/* This Mod only reads the anchors, so it runs after a Mod that moves them (the
   punch-tape Mod's link is at priority 0): the handles then follow the lines the
   player actually sees. */
static constexpr int32_t kAnchorPriority = 100;

static TCPinOrderApiV1 pinOrder;
static TCPinOrderApiV2 pinLayout;
static bool pinOrderReady;
static bool pinLayoutReady;
using DummyFn = void (*)(tc::ui::Vec2);
static DummyFn dummy;
static bool diagLog;
static bool diagMark;
/* The panel draws an entry's own value region with a background of its own once
   the entry grows (a pin with a punch tape is tall enough), and that background
   is submitted *after* the handles - so a handle drawn into the window's own
   list disappears behind the entry.  A window's foreground list is rendered
   after the window's content, which is where an overlay belongs. */
static void* (*foregroundDrawList)(void*) = nullptr;
static void* (*currentWindow)() = nullptr;
static void (*windowPos)(tc::ui::Vec2*) = nullptr;
static void (*windowSize)(tc::ui::Vec2*) = nullptr;

/* One entry of the panel, as a frame laid it out. */
/* Both cursor calls are loader hook chain points, because more than one Mod
   needs them: the punch-tape Mod moves the lines under a taller entry, this one
   draws handles on the entries the panel lays out.  Joining the chain (instead
   of hooking the engine functions) is what lets the two run together, and the
   chain hands over the caller, which is how the panel's own calls are told
   apart from everyone else's. */
struct Row {
    uint32_t group = 0;
    uint32_t index = 0;
    uint64_t key = 0;
    uint64_t width = 0;
    float labelY = 0.f;         /* the label line's centre, in the panel's layout */
    float squaresY = 0.f;       /* the bit squares' line, 0 when the pin has none */
    float squareHeight = 0.f;   /* measured native bit control, not inferred from row spacing */
    float screenY = 0.f;        /* where labelY is on screen */
    float screenX = 0.f;        /* the frame's left edge on screen */
    float panelWidth = 0.f;
    float bandEndY = 0.f;       /* the line the next entry would start on */
    int32_t frame = -1;         /* frame whose contributed control bounds belong here */
    void* window = nullptr;     /* the window the entry was laid out in */
    float windowLeft = 0.f;     /* that window's screen rectangle, so a frame can be */
    float windowTop = 0.f;      /* clipped to the panel without trusting whatever */
    float windowRight = 0.f;    /* clip rectangle happens to be current when it is drawn */
    float windowBottom = 0.f;
    int outcome = 0;            /* what drawRow did with this row (1 drawn, else the reason) */
    bool hasSquares = false;
    bool hasBand = false;
    /* The last entry of its group: what follows it is the panel's own tail, not
       another entry, so the room the panel leaves there is not this entry's
       content. */
    bool last = false;
};

static std::vector<Row> rows;
static std::vector<Row> painted;   /* the previous frame's entries, complete */
static int rowsFrame = -1;
static float rhythm[TC_PIN_ORDER_GROUP_COUNT] = {0.f, 0.f, 0.f};
/* How the last frame's rows ended: 1 drawn, negative the reason they were not.
   The summary prints these, because "the Mod thinks it drew it" and "the screen
   shows it" are two different questions. */
static int lastDraw[TC_PIN_ORDER_GROUP_COUNT] = {0, 0, 0};
static int lastSkip[TC_PIN_ORDER_GROUP_COUNT] = {0, 0, 0};

/* Hover and the "move N slots" window the right button opens. */
static uint64_t hoverKey;
static int hoverDirection;
static uint32_t stepGroup = 0;
static uint32_t stepIndex = 0;
static uint64_t stepKey = 0;
static int stepDirection = -1;
static int stepCount = 1;
static bool stepOpen = false;
static int diagLines;

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static void reportStatus(const std::string& message, int level) {
    if (host && host->report_status) host->report_status(host->context, level, message.c_str());
}

static const char* groupName(uint32_t group) {
    switch (group) {
        case TC_PIN_ORDER_GROUP_INPUTS: return "inputs";
        case TC_PIN_ORDER_GROUP_OUTPUTS: return "outputs";
        default: return "memory";
    }
}

/* Development aid (TC_MODLOADER_PIN_ORDER_LOG=1).  The panel lays its entries
   out through a handful of call sites this Mod measured; a site it has never
   seen is either a section it does not handle yet (the workshop's memory group)
   or a site that moved in a game update - and a missing handle is what either
  of those looks like from outside.  Each site is reported once, so a new build
  can be re-measured from the log instead of by guessing. */
static void noteUnknownAnchor(uintptr_t offset) {
    if (!diagLog) return;
    static uintptr_t seen[32]{};
    static int count = 0;
    for (int index = 0; index < count; ++index)
        if (seen[index] == offset) return;
    if (count >= static_cast<int>(sizeof(seen) / sizeof(seen[0]))) return;
    seen[count++] = offset;
    char line[160];
    std::snprintf(line, sizeof(line), "pin order: panel anchor +0x%llx is not handled",
                  static_cast<unsigned long long>(offset));
    report(line);
}

/* The rhythm of one entry: measured from two entries of the group, or scaled
   off the panel's width while a group has only one entry to look at. */
static float entryRhythm(uint32_t group, float panelWidth) {
    if (group < TC_PIN_ORDER_GROUP_COUNT && rhythm[group] > 8.f) return rhythm[group];
    float fallback = panelWidth * kRhythmPerWidth;
    if (fallback < 40.f) fallback = 40.f;
    if (fallback > 400.f) fallback = 400.f;
    return fallback;
}

/* One line per entry, printed while it says something new: tests/pin-order-
   driver.cpp reads these lines to aim a real drag at the grips, so a fixed
   number of lines cannot do - a 64-bit pin logs square lines of its own, and a
   count that runs out mid-group hides exactly the entries the drag needs.  The
   name is a pin's own text, so control characters are folded away before it
   goes into a line-based log. */
static void diagEntry(const Row& row, const std::string& name) {
    if (!diagLog) return;
    /* Bounded: scrolling changes every row's line, and a session is long. */
    static int written = 0;
    if (written >= 400) return;
    std::string printable;
    printable.reserve(name.size());
    for (char character : name)
        printable.push_back(static_cast<unsigned char>(character) < 32 ? ' ' : character);
    static std::vector<std::string> previous;
    const std::size_t slot = static_cast<std::size_t>(row.group) * 1024 + row.index;
    if (slot >= previous.size()) previous.resize(slot + 1);
    char line[240];
    std::snprintf(line, sizeof(line),
                  "pin order: %s #%u key=%llu width=%llu name=\"%s\" labelY=%.0f "
                  "rhythm=%.0f screen=(%.0f,%.0f) panel=%.0f win=(%.0f,%.0f)-(%.0f,%.0f)",
                  groupName(row.group), row.index, static_cast<unsigned long long>(row.key),
                  static_cast<unsigned long long>(row.width), printable.c_str(), row.labelY,
                  entryRhythm(row.group, row.panelWidth), row.screenX, row.screenY,
                  row.panelWidth, row.windowLeft, row.windowTop, row.windowRight,
                  row.windowBottom);
    if (previous[slot] == line) return;
    previous[slot] = line;
    report(line);
}

/* The frame an entry gets: half a line above its label, and below the squares
   far enough to contain their measured ImGui item rectangle.  A wider pin may
   add one more line for its value.  A pin with no squares at all is either a
   single-bit output - one line, nothing else - or a wide pin, whose value field
   takes the second line instead. */
static void rowFrame(const Row& row, float* top, float* bottom) {
    const float rhythmHere = entryRhythm(row.group, row.panelWidth);
    /* The distance from the label line to the squares' top-left anchor.  Half
       of it remains a suitable margin above the label; the lower edge is based
       on the measured item height below. */
    const float line = row.hasSquares ? row.squaresY - row.labelY : rhythmHere * kLinePerRhythm;
    const float above = line * 0.5f;
    /* The square anchor is its top-left corner, not its centre.  In particular,
       the one-bit control is much taller than half the label-to-square spacing.
       The call site used by captureSquares runs immediately after the game's
       InvisibleButton, so squareHeight is the actual ImGui item height. */
    float fromLabel = tc_pin_order_layout::contentBelowLabel(
        row.hasSquares, line, row.squareHeight, row.width, row.group, rhythmHere,
        kLinePerRhythm, kFallbackBelowPerRhythm, kReportedBoundsPadding);
    /* When another Mod makes an entry taller than the panel sized it for (the
       punch-tape Mod draws its tape inside a wide input, for one), the panel
       puts the next entry that much further down.  That is the entry's real
       extent, and the frame follows it instead of the shape above.  Only for an
       entry another one follows: after the last entry the panel simply ends, and
       hanging the frame off that would make the last handle taller than its
       content. */
    if (!row.last && row.hasBand && row.bandEndY - row.labelY > rhythmHere * kGrownBand)
        fromLabel = std::max(fromLabel, row.bandEndY - row.labelY - rhythmHere * kBandGapPerRhythm);
    *top = row.screenY - above;
    *bottom = row.screenY + fromLabel;
    /* V2 is the loader-owned layout boundary: any Mod that draws controls in
       this entry reports their real rectangle, and the outer frame takes the
       union.  This is what makes the last entry fit too; it no longer needs a
       fictional "next row" from which to infer its content. */
    if (pinLayoutReady) {
        TCPinOrderBoundsV1 contributed{};
        if (tc::pin_order::bounds(pinLayout, row.frame, row.group, row.key, &contributed) ==
            TC_PIN_ORDER_OK) {
            *top = std::min(*top, contributed.min_y - kReportedBoundsPadding);
            *bottom = std::max(*bottom, contributed.max_y + kReportedBoundsPadding);
        }
    }
    /* The label line has to fit inside the frame even where the panel gives an
       entry no second line to hang the frame off. */
    float minimum = tc::ui::frameHeight();
    if (minimum < 24.f) minimum = 24.f;
    if (*bottom - *top < minimum) *bottom = *top + minimum;
}

/* A draw-list rectangle is not an ImGui item, so it does not extend the child
   window's CursorMaxPos.  Without this sentinel the scrollbar stops at the
   last bit control and clips the padding plus the frame's bottom edge.  The
   previous frame has complete row geometry; reserve its lowest local Y in the
   current child, then restore the game's cursor before it draws anything. */
static void reserveFrameExtent() {
    if (!dummy || painted.empty()) return;
    float lowest = 0.f;
    for (const Row& row : painted) {
        float top = 0.f, bottom = 0.f;
        rowFrame(row, &top, &bottom);
        const float localBottom = row.labelY + (bottom - row.screenY);
        if (localBottom > lowest) lowest = localBottom;
    }
    if (lowest <= 0.f) return;
    const tc::ui::Vec2 saved = tc::ui::cursorPos();
    tc::ui::setCursorPos({saved.x, lowest});
    dummy({1.f, 1.f});
    tc::ui::setCursorPos(saved);
}

/* The two buttons, in screen coordinates: up above the label line, down below
   it.  Both are squares of the same size, so the hit test and the drawing can
   share this one function. */
static void buttonRects(const Row& row, tc::ui::Vec2* up, tc::ui::Vec2* down) {
    const float left = row.screenX + kButtonLeft;
    const float label = row.screenY;
    *up = {left, label - kButtonGap - kButtonSize};
    *down = {left, label + kButtonGap};
}

/* Which button a point is on: -1 up, +1 down, 0 neither. */
static int buttonAt(const Row& row, tc::ui::Vec2 point) {
    tc::ui::Vec2 up{}, down{};
    buttonRects(row, &up, &down);
    const tc::ui::Vec2 corners[2] = {up, down};
    const int directions[2] = {-1, 1};
    for (int index = 0; index < 2; ++index) {
        const tc::ui::Vec2& corner = corners[index];
        if (point.x >= corner.x && point.x <= corner.x + kButtonSize &&
            point.y >= corner.y && point.y <= corner.y + kButtonSize)
            return directions[index];
    }
    return 0;
}

/* The frame, the grip strip and the drop marker: pure draw-list work in the
   panel's own window, so nothing is submitted as an item and no mouse state is
   consumed.  Returns 1 when the row was drawn, and a negative reason otherwise
   (the diagnostic summary counts them, so "the frame is missing" can be told
   apart from "the frame was drawn"). */
static int drawRow(const Row& row, bool hasLimit, float limit) {
    if (!tc::ui::drawingReady()) return -1;
    auto& api = tc::ui::drawing_detail::table();
    /* The window the panel draws in, not the viewport's foreground layer: the
       panel's own clip then trims entries that scrolled out of it.  (Drawing
       them on the foreground layer was tried and is wrong - an entry below the
       panel painted its buttons over the drawer underneath.) */
    void* list = api.windowList ? api.windowList() : nullptr;
    if (!list) return -2;
    float top = 0.f, bottom = 0.f;
    rowFrame(row, &top, &bottom);
    /* An entry that grew must not paint over the entry below it. */
    if (hasLimit && limit > top + 8.f && bottom > limit) bottom = limit;
    const float left = row.screenX;
    const float right = row.screenX + (row.panelWidth - kFrameInset);
    if (!(bottom > top + 4.f) || !(right > left + 8.f)) return -3;
    const bool hovered = row.key != 0 && row.key == hoverKey;
    using tc::ui::rgba;
    api.pushClip(list, {left - 2.f, top - 2.f}, {right + 2.f, bottom + 2.f}, true);
    const uint32_t outline = hovered ? rgba(150, 200, 250, 200) : rgba(120, 120, 140, 90);
    api.rect(list, {left, top}, {right, bottom}, outline, 6.f, 0,
             hovered ? 2.f : 1.f);
    /* The two move buttons.  Their hit test is our own arithmetic, so they keep
       working even where a frame's outline is hard to see. */
    tc::ui::Vec2 up{}, down{};
    buttonRects(row, &up, &down);
    const tc::ui::Vec2 corners[2] = {up, down};
    const int directions[2] = {-1, 1};
    for (int index = 0; index < 2; ++index) {
        const tc::ui::Vec2 corner = corners[index];
        const tc::ui::Vec2 end{corner.x + kButtonSize, corner.y + kButtonSize};
        const bool lit = hovered && hoverDirection == directions[index];
        api.rectFilled(list, corner, end,
                       lit ? rgba(120, 170, 220, 235) : rgba(96, 96, 116, 200), 3.f, 0);
        api.rect(list, corner, end, rgba(190, 200, 220, 150), 3.f, 0, 1.f);
        const float middle = corner.x + kButtonSize * 0.5f;
        const float head = corner.y + (directions[index] < 0 ? 4.f : kButtonSize - 4.f);
        const float base = corner.y + (directions[index] < 0 ? kButtonSize - 4.f : 4.f);
        api.triangleFilled(list, {middle, head}, {middle - 4.5f, base}, {middle + 4.5f, base},
                           rgba(235, 240, 250, 235));
    }
    api.popClip(list);
    return 1;
}
static void measureRhythm() {
    for (size_t index = 1; index < painted.size(); ++index) {
        const Row& previous = painted[index - 1];
        const Row& row = painted[index];
        if (row.group != previous.group) continue;
        const float delta = row.labelY - previous.labelY;
        if (delta > 8.f && delta < 4096.f) rhythm[row.group] = delta;
    }
}

/* Where an entry key sits now: the window's buttons are pressed on later frames
   than the click that opened it, and the entry may have moved since. */
static bool indexOfKey(uint32_t group, uint64_t key, uint32_t* out) {
    const uint32_t count = tc::pin_order::count(pinOrder, group);
    for (uint32_t index = 0; index < count; ++index) {
        TCPinOrderEntryV1 entry{};
        if (tc::pin_order::entry(pinOrder, group, index, &entry) != TC_PIN_ORDER_OK) continue;
        if (entry.key != key) continue;
        if (out) *out = index;
        return true;
    }
    return false;
}

/* Development aid (TC_MODLOADER_PIN_ORDER_LOG=1): one line per frame, and only
   while it says something new.  "A group where some pins have no handle" is a
   frame count that does not match the panel's own count, so both are printed
   side by side with the geometry of every entry - the numbers that say whether
   the cache is short, the anchors were missed, or the frame is off screen. */
static void diagSummary() {
    if (!diagLog || !pinOrderReady) return;
    char line[384];
    int written = std::snprintf(line, sizeof(line), "pin order: summary");
    for (uint32_t group = 0; group < TC_PIN_ORDER_GROUP_COUNT; ++group) {
        int counted = 0;
        for (const Row& row : painted)
            if (row.group == group) ++counted;
        if (written < 0 || written >= static_cast<int>(sizeof(line))) break;
        const int added = std::snprintf(line + written, sizeof(line) - written,
                                       " | %s cache=%u rows=%d drawn=%d skipped=%d",
                                       groupName(group),
                                       tc::pin_order::count(pinOrder, group), counted,
                                       lastDraw[group], lastSkip[group]);
        written += added > 0 ? added : 0;
    }
    static std::string previous;
    const std::string summary(line);
    if (summary == previous) return;
    previous = summary;
    static int reported = 0;
    if (reported >= 200) return;
    ++reported;
    report(summary);
    for (const Row& row : painted) {
        float top = 0.f;
        float bottom = 0.f;
        rowFrame(row, &top, &bottom);
        char detail[224];
        std::snprintf(detail, sizeof(detail),
                      "pin order: row %s #%u key=%llu width=%llu top=%.0f bottom=%.0f "
                      "x=%.0f w=%.0f squares=%d h=%.1f draw=%d win=(%.0f,%.0f)-(%.0f,%.0f)",
                      groupName(row.group), row.index,
                      static_cast<unsigned long long>(row.key),
                      static_cast<unsigned long long>(row.width), top, bottom, row.screenX,
                      row.panelWidth, row.hasSquares ? 1 : 0,
                      static_cast<double>(row.squareHeight), row.outcome,
                      row.windowLeft, row.windowTop, row.windowRight, row.windowBottom);
        report(detail);
    }
}

/* One move of `slots` places, clamped at the ends of the group.  Returns true
   when the order really changed. */
static bool moveBySlots(uint32_t group, uint32_t index, int slots) {
    const uint32_t count = tc::pin_order::count(pinOrder, group);
    if (count == 0 || index >= count) return false;
    int target = static_cast<int>(index) + slots;
    if (target < 0) target = 0;
    if (target >= static_cast<int>(count)) target = static_cast<int>(count) - 1;
    if (target == static_cast<int>(index)) return false;
    const int status = tc::pin_order::move(pinOrder, group, index,
                                           static_cast<uint32_t>(target));
    char line[192];
    std::snprintf(line, sizeof(line), "pin order: moved %s #%u to #%d (status %d)",
                  groupName(group), index, target, status);
    report(line);
    return status == TC_PIN_ORDER_OK;
}

/* Hover and clicks, once per frame, from the entries the previous frame painted
   - a frame's entries are only complete once their last one has been drawn, and
   one frame of lag is not something a pointer can notice.  A left click moves
   one slot, a right click opens the "how many slots" window, Ctrl+left restores
   the panel's own order. */
static void interact() {
    if (!pinOrderReady) return;
    const tc::ui::Vec2 mouse = tc::ui::mousePos();
    hoverKey = 0;
    hoverDirection = 0;
    if (painted.empty()) return;
    const Row* hovered = nullptr;
    int direction = 0;
    for (const Row& row : painted) {
        const int hit = buttonAt(row, mouse);
        if (!hit) continue;
        hovered = &row;
        direction = hit;
        hoverKey = row.key;
        hoverDirection = hit;
    }
    if (!hovered) return;
    if (tc::ui::isMouseClicked(tc::ui::Mouse_Left)) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            const int status = tc::pin_order::reset(pinOrder, hovered->group);
            report("pin order: " + std::string(groupName(hovered->group)) +
                   " order reset to the panel's own (status " + std::to_string(status) + ")");
            return;
        }
        moveBySlots(hovered->group, hovered->index, direction);
        return;
    }
    if (tc::ui::isMouseClicked(tc::ui::Mouse_Right)) {
        stepGroup = hovered->group;
        stepIndex = hovered->index;
        stepKey = hovered->key;
        stepDirection = direction;
        stepCount = 1;
        stepOpen = true;
        report("pin order: the slot window is open for " +
               std::string(groupName(stepGroup)) + " #" + std::to_string(stepIndex));
    }
}

/* The window a right click opens: "move this pin N slots up or down".  A real
   ImGui window of this Mod's own, drawn from the frame callback - it is
   modal-free and never touches the panel's layout. */
static void drawStepWindow() {
    if (!stepOpen) return;
    tc::ui::Window window = tc::ui::panel("引脚顺序：移动格数", &stepOpen, {0.f, 0.f}, {-1.f, -1.f},
                                          0.f, tc::ui::Window_AlwaysAutoResize, tc::ui::Cond_Once);
    if (!window) return;
    uint32_t current = stepIndex;
    const bool known = indexOfKey(stepGroup, stepKey, &current);
    tc::ui::text(std::string("组：") + groupName(stepGroup) + "　当前第 " +
                 std::to_string(current + 1) + " 条");
    tc::ui::text(std::string("点击目标：") + (stepDirection < 0 ? "上" : "下") + "移");
    float slots = static_cast<float>(stepCount);
    if (tc::ui::sliderFloat("格数", &slots, 1.f, 32.f, "%.0f")) {
        stepCount = static_cast<int>(slots + 0.5f);
        if (stepCount < 1) stepCount = 1;
        if (stepCount > 32) stepCount = 32;
    }
    tc::ui::separator();
    const std::string label = "移动 " + std::to_string(stepCount) + " 格";
    if (tc::ui::button(label.c_str()) && known) {
        moveBySlots(stepGroup, current, stepDirection * stepCount);
    }
    tc::ui::sameLine();
    if (tc::ui::button("恢复面板顺序")) {
        tc::pin_order::reset(pinOrder, stepGroup);
        report("pin order: " + std::string(groupName(stepGroup)) +
               " order reset to the panel's own");
    }
    tc::ui::sameLine();
    if (tc::ui::button("关闭")) stepOpen = false;
    tc::ui::text("提示：左键点上/下按钮移动一格，Ctrl+左键恢复默认顺序。");
}

/* The Mod's own frame callback: the right click's window belongs to the end of
   the frame, after the game's UI. */
static void frame(void*, const TCFrame*) {
    drawStepWindow();
}

/* Whether this call site is a label anchor, and which group it belongs to. */
static bool labelSite(uintptr_t offset, uint32_t* group) {
    for (uintptr_t site : kInputLabelSites)
        if (offset == site) { *group = TC_PIN_ORDER_GROUP_INPUTS; return true; }
    for (uintptr_t site : kOutputLabelSites)
        if (offset == site) { *group = TC_PIN_ORDER_GROUP_OUTPUTS; return true; }
    return false;
}

/* Only these four panel call sites place bit squares.  Treating every other
   SetCursorPos call as a square corrupts the last row: unlike earlier rows it
   has no following label to replace rows.back(), so later panel layout keeps
   increasing its squaresY and moves the frame bottom out of the viewport. */
static bool squareSite(uintptr_t offset) {
    for (uintptr_t site : kSquareSites)
        if (offset == site) return true;
    return false;
}

/* One entry's label line.  The entries of a group arrive top to bottom, so the
   number of them so far is the entry's place in the group - the index the
   service addresses it by. */
static void captureLabel(float localY, uint32_t group) {
    if (!pinOrderReady) return;
    if (!rows.empty() && rows.back().group == group && std::fabs(rows.back().labelY - localY) < 1.f)
        return;   /* the label's own second call site repeats the same line */
    uint32_t index = 0;
    for (const Row& existing : rows)
        if (existing.group == group) ++index;
    TCPinOrderEntryV1 entry{};
    /* The panel is built before the board's pins have been cached, and the
       service answers with what is really cached: an entry it does not know is
       not an entry of the panel, so there is nothing to draw a handle on. */
    if (tc::pin_order::entry(pinOrder, group, index, &entry) != TC_PIN_ORDER_OK) {
        if (diagLog && diagLines++ < 24)
            report("pin order: " + std::string(groupName(group)) + " #" + std::to_string(index) +
                   " not in the panel cache yet (cache holds " +
                   std::to_string(tc::pin_order::count(pinOrder, group)) + ")");
        return;
    }
    Row row{};
    row.group = group;
    row.index = index;
    row.key = entry.key;
    row.width = entry.width;
    row.labelY = localY;
    row.frame = tc::ui::frameCount();
    row.window = currentWindow ? currentWindow() : nullptr;
    if (windowPos && windowSize) {
        tc::ui::Vec2 position{};
        tc::ui::Vec2 size{};
        windowPos(&position);
        windowSize(&size);
        row.windowLeft = position.x;
        row.windowTop = position.y;
        row.windowRight = position.x + size.x;
        row.windowBottom = position.y + size.y;
    }
    row.panelWidth = tc::ui::windowWidth();
    /* Screen coordinates for the frame: put the layout cursor where the frame's
       left edge is, read where that is on screen, and put the cursor back before
       the game draws anything with it.  Going through the engine's own call is
       what turns a panel coordinate into a screen one; a call from this DLL is
       not a panel anchor, so no Mod treats it as one. */
    const tc::ui::Vec2 saved = tc::ui::cursorPos();
    tc::ui::setCursorPos({kFrameLeft, localY});
    const tc::ui::Vec2 screen = tc::ui::cursorScreenPos();
    tc::ui::setCursorPos(saved);
    row.screenX = screen.x;
    row.screenY = screen.y;
    diagEntry(row, entry.name);
    rows.push_back(row);
}

/* The squares a label is followed by: the entry's second line, which is what
   the frame hangs off. */
static void captureSquares(float localY) {
    if (rows.empty()) return;
    Row& row = rows.back();
    if (localY < row.labelY) return;
    row.squaresY = row.hasSquares ? std::max(row.squaresY, localY) : localY;
    /* These four anchors reset the cursor immediately after drawing the bit's
       InvisibleButton.  LastItem is therefore that bit, including the enlarged
       one-bit shape the old half-line estimate cut through. */
    const tc::ui::Vec2 item = tc::ui::itemRectSize();
    if (item.y > 0.f && item.y < 4096.f)
        row.squareHeight = std::max(row.squareHeight, item.y);
    row.hasSquares = true;
    if (diagLog && diagLines++ < 32) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "pin order: bit bounds key=%llu anchorY=%.0f item=%.0fx%.0f",
                      static_cast<unsigned long long>(row.key), localY,
                      static_cast<double>(item.x), static_cast<double>(item.y));
        report(line);
    }
}

/* The panel's scalar anchors: the line the next entry starts on, and the
   section headings.  The first one after an entry's label is that entry's band
   end - the one number that grows when something else makes the entry taller. */
static void captureLine(float localY) {
    if (rows.empty()) return;
    Row& row = rows.back();
    if (row.hasBand || localY <= row.labelY) return;
    /* A one-bit input/output takes a different branch from the multi-bit
       squares above.  It finishes immediately before this per-entry scalar
       anchor, so LastItem is the enlarged native bit control even for the last
       entry in a group.  Custom auto-width pins use raw width 0 but resolve to
       the same one-bit control, so 0 and 1 share this path.  The panel does not
       expose its top-left anchor on this branch; that top is the same
       second-line rhythm used everywhere else. */
    if (row.width <= 1 && !row.hasSquares) {
        const tc::ui::Vec2 item = tc::ui::itemRectSize();
        if (item.y > 0.f && item.y < 4096.f) {
            row.squaresY = row.labelY + entryRhythm(row.group, row.panelWidth) * kLinePerRhythm;
            row.squareHeight = item.y;
            row.hasSquares = true;
            if (diagLog && diagLines++ < 32) {
                char line[176];
                std::snprintf(line, sizeof(line),
                              "pin order: one-bit bounds key=%llu anchorY=%.0f item=%.0fx%.0f",
                              static_cast<unsigned long long>(row.key),
                              static_cast<double>(row.squaresY), static_cast<double>(item.x),
                              static_cast<double>(item.y));
                report(line);
            }
        }
    }
    row.bandEndY = localY;
    row.hasBand = true;
}

/* The panel placing one line of an entry.  The chain hands over the caller, so
   this Mod sees exactly the calls the panel itself made - its own, the punch
   tape's, and any other Mod's, are all somewhere else. */
static int setCursorPosLink(TCHookCall* call) {
    TCHookSetCursorPosArgs* args = tc::hook::setCursorPosArgs(call);
    if (!args || !tc::hook::calledFrom(call, ioStateView, kPanelSize)) return 0;
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(call->caller) - reinterpret_cast<uintptr_t>(ioStateView);
    uint32_t group = 0;
    const bool label = labelSite(offset, &group);
    const int frame = tc::ui::frameCount();
    if (frame != rowsFrame) {
        painted.swap(rows);
        rows.clear();
        rowsFrame = frame;
        /* Which entry is the last of its group: the panel's y after it is its own
           tail rather than another entry's line. */
        for (size_t index = 0; index < painted.size(); ++index) {
            bool last = true;
            for (size_t other = index + 1; other < painted.size(); ++other) {
                if (painted[other].group == painted[index].group) { last = false; break; }
            }
            painted[index].last = last;
        }
        measureRhythm();
        reserveFrameExtent();
        interact();
        for (uint32_t group = 0; group < TC_PIN_ORDER_GROUP_COUNT; ++group) {
            lastDraw[group] = 0;
            lastSkip[group] = 0;
        }
        /* The handles of the entries this frame's panel has just laid out; they
           were complete by the end of the previous frame.  Each frame is kept
           above the next entry's, so a grown entry cannot paint over it. */
        for (size_t index = 0; index < painted.size(); ++index) {
            bool hasLimit = false;
            float limit = 0.f;
            if (index + 1 < painted.size()) {
                float nextTop = 0.f, nextBottom = 0.f;
                rowFrame(painted[index + 1], &nextTop, &nextBottom);
                limit = nextTop - 2.f;
                hasLimit = true;
            }
            const int outcome = drawRow(painted[index], hasLimit, limit);
            painted[index].outcome = outcome;
            if (painted[index].group < TC_PIN_ORDER_GROUP_COUNT) {
                if (outcome > 0) ++lastDraw[painted[index].group];
                else ++lastSkip[painted[index].group];
            }
        }
        diagSummary();
    }
    if (label) captureLabel(args->y, group);
    else if (squareSite(offset)) captureSquares(args->y);
    else noteUnknownAnchor(offset);
    return 0;
}

static int setCursorPosYLink(TCHookCall* call) {
    TCHookSetCursorPosYArgs* args = tc::hook::setCursorPosYArgs(call);
    if (!args || !tc::hook::calledFrom(call, ioStateView, kPanelSize)) return 0;
    captureLine(args->y);
    return 0;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) {
        report("pin order: tc::ui::load failed: " + tc::ui::missing());
        return 3;
    }
    if (!tc::ui::loadDrawing(h)) {
        reportStatus("引脚顺序: 绘图接口不可用: " + tc::ui::drawingMissing(), 1);
        report("pin order: drawing unavailable: " + tc::ui::drawingMissing());
        return 4;
    }
    {
        char flag[8]{};
        /* On by default: the lines below print when something *changes*, which
           is a handful per panel edit, and a report of "some pins have no
           handle" is worth nothing without them.  TC_MODLOADER_PIN_ORDER_LOG=0
           turns them off; any other value (including `dump`) keeps them and
           also asks the loader for the raw cache. */
        const DWORD length = GetEnvironmentVariableA("TC_MODLOADER_PIN_ORDER_LOG", flag, sizeof(flag));
        diagLog = !(length == 1 && flag[0] == '0');
    }
    {
        char flag[8]{};
        diagMark = GetEnvironmentVariableA("TC_MODLOADER_PIN_ORDER_MARK", flag, sizeof(flag)) > 0;
    }
    if (!tc::pin_order::table(h, &pinOrder) || !tc::pin_order::ready(pinOrder)) {
        report("pin order: tc.pin_order service unavailable; the panel is left alone");
        return 0;
    }
    pinOrderReady = true;
    pinLayoutReady = tc::pin_order::table(h, &pinLayout) && tc::pin_order::ready(pinLayout);
    if (!pinLayoutReady)
        report("pin order: V2 entry bounds unavailable; frames use native anchors only");
    if (void* entry = h->engine_proc(h->context, "igDummy")) {
        static_assert(sizeof(dummy) == sizeof(entry));
        std::memcpy(&dummy, &entry, sizeof(dummy));
    } else {
        report("pin order: igDummy unavailable; the last frame cannot extend panel scrolling");
    }
    ioStateView = static_cast<uint8_t*>(
        h->resolve_symbol(h->context, "build_io_state_view__presenterZboard95uiZio95state95view_u100"));
    if (!ioStateView) {
        report("pin order: io state view symbol not found");
        return 5;
    }
    if (void* entry = h->engine_proc(h->context, "igGetForegroundDrawList_WindowPtr")) {
        static_assert(sizeof(foregroundDrawList) == sizeof(entry));
        std::memcpy(&foregroundDrawList, &entry, sizeof(foregroundDrawList));
    }
    if (void* entry = h->engine_proc(h->context, "igGetCurrentWindow")) {
        static_assert(sizeof(currentWindow) == sizeof(entry));
        std::memcpy(&currentWindow, &entry, sizeof(currentWindow));
    }
    if (void* entry = h->engine_proc(h->context, "igGetWindowPos")) {
        static_assert(sizeof(windowPos) == sizeof(entry));
        std::memcpy(&windowPos, &entry, sizeof(windowPos));
    }
    if (void* entry = h->engine_proc(h->context, "igGetWindowSize")) {
        static_assert(sizeof(windowSize) == sizeof(entry));
        std::memcpy(&windowSize, &entry, sizeof(windowSize));
    }
    if (!foregroundDrawList || !currentWindow)
        report("pin order: foreground draw list unavailable; handles use the window list");
    /* Both anchors are loader hook chain points, shared with the Mods that draw
       inside the same panel.  Without them there is nowhere to hang a handle,
       so this is a refusal rather than a degraded mode. */
    const int entryLink = tc::hook::addSetCursorPos(h, kAnchorPriority, &setCursorPosLink, nullptr);
    const int lineLink = tc::hook::addSetCursorPosY(h, kAnchorPriority, &setCursorPosYLink, nullptr);
    if (entryLink != TC_HOOK_OK || lineLink != TC_HOOK_OK) {
        report("pin order: anchor hook chain refused (" + std::to_string(entryLink) + "/" +
               std::to_string(lineLink) + "); " + tc::hook::errorText(entryLink));
        return 6;
    }
    out->on_frame = &frame;
    reportStatus("引脚顺序: 点条目左边的 ▲/▼ 移动一格；右键按钮可设置移动格数（Ctrl+点击恢复默认）", 0);
    report("pin order: entry frame hook installed");
    return 0;
}
