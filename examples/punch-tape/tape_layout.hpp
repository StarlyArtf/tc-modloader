#ifndef TC_PUNCH_TAPE_LAYOUT_HPP
#define TC_PUNCH_TAPE_LAYOUT_HPP
/* Where the punch tape puts its chips.

   Kept apart from the plugin so the arithmetic can be tested offline: the panel
   is the only thing that knows how much room it has, and "the tape follows the
   window" is a property of these functions, not of the drawing code.

   Rules, in the order they apply:

     1. bits are grouped in bytes and up to four groups share a row;
     2. wrappings whose rows are evenly filled are preferred - a tape should not
        end in a stub row while a clean shape fits - and among those the one
        that leaves the biggest chips inside the panel's free box wins;
     3. that choice is scaled into the box, up to kMaxScale and down to
        kMinScale, so a big window gets a bigger tape and a short drawer still
        shows every row.

   `availableHeight` <= 0 means the panel did not report a height: no vertical
   limit is applied. */

namespace tc_tape {

struct Point {
    float x = 0.f;
    float y = 0.f;
};

struct Layout {
    float cell = 44.f;
    float cellGap = 5.f;
    float groupGap = 28.f;
    float rowGap = 24.f;
    float rounding = 11.f;
    int groupsPerRow = 4;
};

inline constexpr int kGroupsPerRow = 4;
inline constexpr int kBitsPerGroup = 8;
inline constexpr Layout kPreferred{44.f, 5.f, 28.f, 24.f, 11.f, kGroupsPerRow};
inline constexpr float kMaxScale = 1.5f;
inline constexpr float kMinScale = 0.25f;

/* Width of a row that holds `bits` bits, capped to the row's capacity. */
inline float rowWidth(int bits, const Layout& layout) {
    if (bits <= 0) return 0.f;
    const int capacity = layout.groupsPerRow * kBitsPerGroup;
    if (bits > capacity) bits = capacity;
    const int groups = (bits + kBitsPerGroup - 1) / kBitsPerGroup;
    const float step = layout.cell + layout.cellGap;
    const float groupWidth = kBitsPerGroup * step - layout.cellGap;
    return groups * groupWidth + (groups > 0 ? (groups - 1) * layout.groupGap : 0.f);
}

inline float gridWidth(int width, const Layout& layout) {
    const int bitsPerRow = layout.groupsPerRow * kBitsPerGroup;
    return rowWidth(width < bitsPerRow ? width : bitsPerRow, layout);
}

inline float gridHeight(int width, const Layout& layout) {
    const int bitsPerRow = layout.groupsPerRow * kBitsPerGroup;
    const int rows = (width + bitsPerRow - 1) / bitsPerRow;
    return rows * layout.cell + (rows > 0 ? (rows - 1) * layout.rowGap : 0.f);
}

/* True when every row of this wrapping is filled to the same number of bits
   (one row, or whole rows).  A 64-bit value in three groups per row would end
   in a 16-bit stub; in four or two it ends on a row boundary. */
inline bool evenRows(int width, int groupsPerRow) {
    const int bitsPerRow = groupsPerRow * kBitsPerGroup;
    if (bitsPerRow <= 0) return false;
    return width <= bitsPerRow || (width % bitsPerRow) == 0;
}

/* The scale this wrapping would use inside the box.  `bounded` is false when
   the panel did not report a height: then the tape keeps the rhythm the panel
   was measured at instead of growing into unknown space. */
inline float fittedScale(int width, const Layout& layout, float availableWidth,
                         float availableHeight, bool bounded, float minimumScale,
                         float maximumScale) {
    const float row = gridWidth(width, layout);
    const float height = gridHeight(width, layout);
    float scale = maximumScale;
    if (row > 0.f && availableWidth / row < scale) scale = availableWidth / row;
    if (bounded && height > 0.f && availableHeight / height < scale)
        scale = availableHeight / height;
    if (!bounded && scale > 1.f) scale = 1.f;
    if (scale > maximumScale) scale = maximumScale;
    if (scale < minimumScale) scale = minimumScale;
    return scale;
}

/* Fits `width` bits into an `availableWidth` x `availableHeight` box.  The
   optional scale bounds let a compact native side panel use the same byte
   grouping rules without inheriting the much larger bottom drawer's floor; the
   `preferred` rhythm is the shape the fit scales from, so a panel that wants
   tighter rows than the drawer's 24 px can ask for them here instead of
   squeezing the result afterwards. */
inline Layout makeLayout(int width, float availableWidth, float availableHeight,
                         float minimumScale = kMinScale,
                         float maximumScale = kMaxScale,
                         int maximumGroupsPerRow = kGroupsPerRow,
                         const Layout& preferred = kPreferred) {
    if (minimumScale < 0.f) minimumScale = 0.f;
    if (maximumScale < minimumScale) maximumScale = minimumScale;
    if (maximumGroupsPerRow < 1) maximumGroupsPerRow = 1;
    if (maximumGroupsPerRow > kGroupsPerRow)
        maximumGroupsPerRow = kGroupsPerRow;
    const int totalGroups = (width + kBitsPerGroup - 1) / kBitsPerGroup;
    int widest = totalGroups < 1 ? 1 : totalGroups;
    if (widest > maximumGroupsPerRow) widest = maximumGroupsPerRow;
    const bool bounded = availableHeight > 0.f;
    Layout best = preferred;
    best.groupsPerRow = widest;
    float bestScale = -1.f;
    /* Even wrappings first; a stub row is only accepted when nothing else can
       be laid out (which happens for widths that are not a whole number of
       bytes). */
    for (int pass = 0; pass < 2 && bestScale < 0.f; ++pass) {
        for (int groups = widest; groups >= 1; --groups) {
            if (pass == 0 && !evenRows(width, groups)) continue;
            Layout candidate = preferred;
            candidate.groupsPerRow = groups;
            const float scale = fittedScale(width, candidate, availableWidth,
                                            availableHeight, bounded,
                                            minimumScale, maximumScale);
            if (scale > bestScale) {
                bestScale = scale;
                best = candidate;
            }
        }
    }
    if (bestScale < 0.f) bestScale = minimumScale;
    best.cell = preferred.cell * bestScale;
    best.cellGap = preferred.cellGap * bestScale;
    best.groupGap = preferred.groupGap * bestScale;
    best.rowGap = preferred.rowGap * bestScale;
    best.rounding = preferred.rounding * bestScale;
    return best;
}

/* Position of one bit inside the grid.  Within a row the most significant
   visible bit is on the left; a short final row is centred under the full
   rows. */
inline bool cellPositionForBit(int bit, int width, const Layout& layout, Point* out) {
    if (!out || bit < 0 || bit >= width || width <= 0) return false;
    const int bitsPerRow = layout.groupsPerRow * kBitsPerGroup;
    if (bitsPerRow <= 0) return false;
    const int row = bit / bitsPerRow;          /* low chunk first */
    const int firstBit = row * bitsPerRow;
    const int remaining = width - firstBit;
    const int rowBits = remaining < bitsPerRow ? remaining : bitsPerRow;
    const int within = rowBits - 1 - (bit - firstBit);
    const int group = within / kBitsPerGroup;
    const int index = within % kBitsPerGroup;
    const float step = layout.cell + layout.cellGap;
    const float groupWidth = kBitsPerGroup * step - layout.cellGap;
    const float rowOffset =
        (gridWidth(width, layout) - rowWidth(rowBits, layout)) * 0.5f;
    out->x = rowOffset + static_cast<float>(group) * (groupWidth + layout.groupGap) +
             static_cast<float>(index) * step;
    out->y = static_cast<float>(row) * (layout.cell + layout.rowGap);
    return true;
}

} /* namespace tc_tape */

#endif
