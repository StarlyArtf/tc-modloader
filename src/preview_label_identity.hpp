#pragma once

#include <cmath>

namespace tc::preview_labels {

/* A label's centre is not a pin identity: changing the text changes its width
   and therefore moves that centre, even though the pin itself did not move.
   Once the preview has supplied its pin bar, match frames by the stable pin
   endpoint and direction instead.  The centre remains the first-frame fallback
   for the one frame in which no bar has been observed yet. */
inline bool samePin(bool previousHasBar, float previousPinX, float previousPinY,
                    float previousOutwardX, float previousOutwardY,
                    float previousCentreX, float previousCentreY,
                    bool currentHasBar, float currentPinX, float currentPinY,
                    float currentOutwardX, float currentOutwardY,
                    float currentCentreX, float currentCentreY,
                    float tolerance = 2.f) {
    if (previousHasBar && currentHasBar) {
        return std::fabs(previousPinX - currentPinX) < tolerance &&
               std::fabs(previousPinY - currentPinY) < tolerance &&
               std::fabs(previousOutwardX - currentOutwardX) < 0.5f &&
               std::fabs(previousOutwardY - currentOutwardY) < 0.5f;
    }
    return std::fabs(previousCentreX - currentCentreX) < tolerance &&
           std::fabs(previousCentreY - currentCentreY) < tolerance;
}

} // namespace tc::preview_labels
