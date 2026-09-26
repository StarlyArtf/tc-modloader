#pragma once

#include <algorithm>
#include <cstdint>

namespace tc_pin_order_layout {

/* Distance from the label anchor to the bottom of an entry's native content.

   The game's bit control is an InvisibleButton whose real height changes with
   the pin width.  The pin-order Mod reads igGetItemRectSize at the corresponding
   post-widget anchor.  Keep the old line estimate for the optional value line,
   but never let it cut through the measured button. */
inline float contentBelowLabel(bool hasSquares, float squareLineFromLabel,
                               float squareHeight, uint64_t width,
                               uint32_t group, float rhythm,
                               float linePerRhythm, float fallbackBelowPerRhythm,
                               float contentPadding) {
    const float line = hasSquares ? squareLineFromLabel : rhythm * linePerRhythm;
    const float above = line * 0.5f;
    if (hasSquares) {
        const float estimated = line + above + (width > 1 ? line : 0.f);
        const float measuredSquare =
            squareHeight > 0.f ? line + squareHeight + contentPadding : 0.f;
        return std::max(estimated, measuredSquare);
    }
    if (width > 1)
        return group == 0 ? rhythm * fallbackBelowPerRhythm : line + above;
    return line + above;
}

}  // namespace tc_pin_order_layout
