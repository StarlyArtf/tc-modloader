/* Offline test for "the punch tape follows the window".

   The tape is drawn inside the game, but where its chips land is pure
   arithmetic (examples/punch-tape/tape_layout.hpp), so the rule the panel
   promises can be checked without the game:

     * a wide panel grows the chips, capped at 1.5x the preferred size;
     * a narrow panel wraps whole eight-bit groups before shrinking anything;
     * a short drawer scales the grid down until every row fits;
     * a panel with no usable space falls back to the smallest scale;
     * and every bit's cell stays inside the grid the layout reports. */
#include "../examples/punch-tape/tape_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

void checkClose(float actual, float expected, float tolerance, const std::string& what) {
    if (std::fabs(actual - expected) <= tolerance) return;
    ++failures;
    std::printf("FAIL %s (%.3f vs %.3f)\n", what.c_str(), actual, expected);
}

/* One drawer: the panel size and the free box the plugin computes from it. */
struct Drawer {
    float panelWidth;
    float panelHeight;
};

constexpr float kNativeControlsRight = 680.f;
constexpr float kControlsTapeGap = 32.f;
constexpr float kHorizontalMargin = 16.f;
constexpr float kVerticalMargin = 10.f;
constexpr float kTapeTop = 119.f;

struct Box {
    float left;
    float width;
    float height;
};

Box freeBox(const Drawer& drawer) {
    Box box{};
    box.left = kNativeControlsRight + kControlsTapeGap;
    if (box.left > drawer.panelWidth * 0.45f) box.left = drawer.panelWidth * 0.45f;
    box.width = drawer.panelWidth - box.left - kHorizontalMargin;
    box.height = drawer.panelHeight > 0.f ? drawer.panelHeight - kTapeTop - kVerticalMargin : -1.f;
    return box;
}

/* Every bit inside the grid, in row order, and nothing outside it. */
void checkBitsInsideGrid(int width, const tc_tape::Layout& layout, const std::string& label) {
    const float gridWidth = tc_tape::gridWidth(width, layout);
    const float gridHeight = tc_tape::gridHeight(width, layout);
    for (int bit = 0; bit < width; ++bit) {
        tc_tape::Point point{};
        check(tc_tape::cellPositionForBit(bit, width, layout, &point),
              label + ": bit " + std::to_string(bit) + " has a cell");
        check(point.x >= -0.01f && point.x + layout.cell <= gridWidth + 0.01f &&
                  point.y >= -0.01f && point.y + layout.cell <= gridHeight + 0.01f,
              label + ": bit " + std::to_string(bit) + " stays inside the grid");
    }
    check(tc_tape::gridWidth(width, layout) > 0.f, label + ": the grid has a width");
}

}  // namespace

int main() {
    /* The machine this was written on: a maximised window, 64-bit and 18-bit
       constants.  The tape has to use the room it has, not the 44px rhythm it
       was first measured at. */
    const Drawer wide{2555.f, 345.f};
    {
        const Box box = freeBox(wide);
        const tc_tape::Layout small = tc_tape::makeLayout(18, box.width, box.height);
        const tc_tape::Layout big = tc_tape::makeLayout(64, box.width, box.height);
        checkClose(small.cell, tc_tape::kPreferred.cell * tc_tape::kMaxScale, 0.01f,
                   "a wide drawer grows the chips to the cap");
        check(small.groupsPerRow == 3, "18 bits use three groups on one row");
        check(tc_tape::gridWidth(18, small) <= box.width + 0.01f,
              "the grown 18-bit tape still fits the free width");
        check(tc_tape::gridHeight(18, small) <= box.height + 0.01f,
              "the grown 18-bit tape still fits the free height");
        check(small.cell > tc_tape::kPreferred.cell,
              "the grow direction is real, not a no-op");
        check(big.cell > tc_tape::kPreferred.cell * 0.9f,
              "64 bits do not shrink on a wide drawer");
        check(tc_tape::gridWidth(64, big) <= box.width + 0.01f,
              "the 64-bit tape fits the free width");
        check(tc_tape::gridHeight(64, big) <= box.height + 0.01f,
              "the 64-bit tape fits the free height");
        checkBitsInsideGrid(18, small, "wide/18");
        checkBitsInsideGrid(64, big, "wide/64");
    }

    /* The window the layout was first measured in: 1462x914, a drawer around
       300px tall.  The chips should sit at the preferred size or slightly
       under. */
    {
        const Box box = freeBox(Drawer{1462.f, 300.f});
        const tc_tape::Layout layout = tc_tape::makeLayout(64, box.width, box.height);
        check(layout.cell <= tc_tape::kPreferred.cell + 0.01f,
              "a small window never grows past the preferred size");
        check(tc_tape::gridWidth(64, layout) <= box.width + 0.01f,
              "64 bits fit a narrow drawer's width");
        check(tc_tape::gridHeight(64, layout) <= box.height + 0.01f,
              "64 bits fit a short drawer's height");
        checkBitsInsideGrid(64, layout, "small/64");
    }

    /* A narrow panel wraps whole byte groups before it shrinks anything: with
       room for three groups at the preferred size, a 64-bit value wraps to two
       groups per row (four evenly filled rows) and keeps the preferred chips. */
    {
        const tc_tape::Layout layout = tc_tape::makeLayout(64, 3.f * 387.f + 2.f * 28.f, -1.f);
        check(layout.groupsPerRow == 2, "a row that does not fit wraps to an even shape");
        checkClose(layout.cell, tc_tape::kPreferred.cell, 0.01f,
                   "wrapping happens before any shrinking");
        checkBitsInsideGrid(64, layout, "wrapped/64");
    }

    /* Rows are filled evenly whenever that is possible: 64 bits never become
       three rows of 24/24/16 just because the chips would be bigger. */
    {
        const Box box = freeBox(wide);
        const tc_tape::Layout layout = tc_tape::makeLayout(64, box.width, box.height);
        check(tc_tape::evenRows(64, layout.groupsPerRow),
              "a 64-bit tape ends on a row boundary");
        check(tc_tape::evenRows(32, tc_tape::makeLayout(32, box.width, box.height).groupsPerRow),
              "a 32-bit tape ends on a row boundary");
    }

    /* A drawer that is far too short: the grid shrinks (not below the floor)
       instead of letting the last row leave the panel. */
    {
        const Box box = freeBox(Drawer{2555.f, 180.f});
        const tc_tape::Layout layout = tc_tape::makeLayout(64, box.width, box.height);
        check(tc_tape::gridHeight(64, layout) <= box.height + 0.01f,
              "a short drawer still shows every row");
        check(layout.cell >= tc_tape::kPreferred.cell * tc_tape::kMinScale - 0.01f,
              "the chips never shrink below the floor");
    }

    /* Degenerate panels: no width, no height, one bit, the widest value. */
    {
        const tc_tape::Layout none = tc_tape::makeLayout(64, 0.f, 0.f);
        checkClose(none.cell, tc_tape::kPreferred.cell * tc_tape::kMinScale, 0.01f,
                   "no room falls back to the smallest scale");
        const tc_tape::Layout negative = tc_tape::makeLayout(64, -50.f, -1.f);
        checkClose(negative.cell, tc_tape::kPreferred.cell * tc_tape::kMinScale, 0.01f,
                   "a negative box falls back to the smallest scale");
        const tc_tape::Layout one = tc_tape::makeLayout(1, 400.f, -1.f);
        check(one.groupsPerRow == 1, "a one-bit value uses one group");
        checkBitsInsideGrid(1, one, "one-bit");
        const tc_tape::Layout full = tc_tape::makeLayout(64, 4000.f, 4000.f);
        checkClose(full.cell, tc_tape::kPreferred.cell * tc_tape::kMaxScale, 0.01f,
                   "an enormous panel still caps the growth");
        checkBitsInsideGrid(64, full, "capped/64");
    }

    /* Grouping is by bytes, and the rows run low chunk first. */
    {
        const tc_tape::Layout layout = tc_tape::makeLayout(64, 4000.f, -1.f);
        tc_tape::Point low{}, high{};
        check(tc_tape::cellPositionForBit(0, 64, layout, &low), "bit 0 has a cell");
        check(tc_tape::cellPositionForBit(33, 64, layout, &high), "bit 33 has a cell");
        check(high.y > low.y, "higher chunks sit on lower rows");
        check(low.x > high.x, "the least significant bit is the rightmost of its row");
    }

    /* The component-workshop input panel is much narrower than the bottom
       drawer, and it does not stack its entries: the game anchors each entry on
       a fixed rhythm, so the tape cannot make itself room by drawing taller.
       The plugin instead adds the tape's height to that anchor, which pushes
       the entries below it - and, after the last one, the outputs heading and
       the whole outputs list - down by exactly the room the tape took.  The
       layout half of that is just the tape the sidebar band asks for; keeping
       the native gap means the pushed anchor is `rhythm + tape + gap`, which
       has to cover `label + tape + gap + value box`. */
    constexpr float kSideRhythm = 121.f;   /* measured on the 333 px sidebar */
    constexpr float kSideLabel = 18.f;
    constexpr float kSideValueBox = 58.f;
    constexpr float kSideGap = 6.f;        /* the canvas gap under the tape */
    constexpr float kSideWidth = 333.f - 2.f * 10.f;
    {
        /* Tall panel (>430 px): the band the sidebar uses there, which is what
           produced the hole size players see today. */
        constexpr float kTallBand = 110.f;
        tc_tape::Layout preferred = tc_tape::kPreferred;
        preferred.rowGap = preferred.cell * 0.25f;
        preferred.groupGap = preferred.cell * 0.4f;
        const tc_tape::Layout wide64 =
            tc_tape::makeLayout(64, kSideWidth, kTallBand, 0.04f, 20.f / 44.f, 2, preferred);
        check(wide64.groupsPerRow == 2,
              "a 64-bit workshop input uses at most two byte groups per row");
        check(wide64.cell > 16.f,
              "the sidebar tape keeps the hole size it is known for");
        check(tc_tape::gridWidth(64, wide64) <= kSideWidth + 0.01f,
              "the workshop tape fits the side-panel width");
        check(wide64.rowGap <= wide64.cell * 0.25f + 0.01f,
              "workshop row spacing is at most one quarter of a hole");
        check(wide64.groupGap <= wide64.cell * 0.4f + 0.01f,
              "workshop byte groups keep the tighter gap of the sidebar");
        checkBitsInsideGrid(64, wide64, "workshop/64");

        /* Once the entries below a tape need the panel to scroll, ImGui keeps
           the scrollbar inside the window and the tape has to be sized against
           what is left of it - the last byte group used to hide under the bar.
           Both numbers are the plugin's: the window's content region on the
           right and the same 10 px margin on each side. */
        constexpr float kScrollbar = 14.f;
        const float scrolledWidth = kSideWidth + 2.f * 10.f - kScrollbar - 2.f * 10.f;
        const tc_tape::Layout scrolled =
            tc_tape::makeLayout(64, scrolledWidth, kTallBand, 0.04f, 20.f / 44.f, 2, preferred);
        check(scrolled.groupsPerRow == 2,
              "a scrolled sidebar still wraps into even byte groups");
        check(tc_tape::gridWidth(64, scrolled) <= scrolledWidth + 0.01f,
              "the tape stays clear of the scrollbar");
        check(scrolled.cell < wide64.cell,
              "clearing the scrollbar costs a little hole size, not a column");
        checkBitsInsideGrid(64, scrolled, "workshop-scrolled/64");

        /* Pushing the next entry down by the tape keeps the native spacing and
           still leaves the value box inside its own entry. */
        const float tape = tc_tape::gridHeight(64, wide64);
        const float pushedAnchor = kSideRhythm + tape + kSideGap;
        const float contentEnd = kSideLabel + tape + kSideGap + kSideValueBox;
        check(contentEnd <= pushedAnchor + 0.01f,
              "a 64-bit entry with its value box fits under the pushed anchor");
        checkClose(pushedAnchor - contentEnd, kSideRhythm - kSideLabel - kSideValueBox, 0.01f,
                   "the gap under the value box is the native one");
        check(kSideLabel + kSideValueBox <= kSideRhythm + 0.01f,
              "the native entry this is measured against fits its own rhythm");

        /* A short in-level panel (>8-bit input, panel under 430 px) keeps the
           compact band, and its 32-bit tape stays two rows of two groups. */
        constexpr float kShortBand = 40.f;
        const tc_tape::Layout side32 =
            tc_tape::makeLayout(32, kSideWidth, kShortBand, 0.04f, 20.f / 44.f, 2, preferred);
        check(side32.groupsPerRow == 2, "32 bits use two byte groups per row");
        check(side32.cell > 15.f,
              "the compact band still gives the 32-bit tape usable holes");
        const float tape32 = tc_tape::gridHeight(32, side32);
        check(kSideLabel + tape32 + kSideGap + kSideValueBox <=
                  kSideRhythm + tape32 + kSideGap + 0.01f,
              "a 32-bit entry fits under its pushed anchor too");
        checkBitsInsideGrid(32, side32, "workshop/32");
    }

    if (failures) {
        std::printf("%d punch tape layout check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS punch tape layout: wrap first, then scale into the panel, both ways\n");
    return 0;
}
