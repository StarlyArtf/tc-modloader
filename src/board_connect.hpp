#pragma once
#include "../sdk/tc_service_api.h"

namespace tc::board_connect {
/* Pins sit close to their component: the widest relative offset in the pinned
   build's own tables is about ±20 grid units (see build/kinds.txt).  A wire end
   only has to be compared against components within this window, which keeps the
   query linear in the number of nearby components instead of the whole board. */
inline constexpr int32_t kSearchWindow=32;

inline bool withinWindow(int32_t wireX,int32_t wireY,int32_t componentX,int32_t componentY){
    const int32_t dx=wireX-componentX,dy=wireY-componentY;
    return dx>=-kSearchWindow&&dx<=kSearchWindow&&dy>=-kSearchWindow&&dy<=kSearchWindow;
}
/* The campaign level's wire from (9,0) to (-9,0) ends exactly on the output
   pin's input port (10,0)+(-1,0) and the input pin's output port
   (-10,0)+(1,0). */
inline bool pinSitsOnPoint(int32_t wireX,int32_t wireY,int32_t componentX,int32_t componentY,
                           int32_t pinOffsetX,int32_t pinOffsetY){
    return componentX+pinOffsetX==wireX&&componentY+pinOffsetY==wireY;
}
inline void fillEnd(TCWireEndV1& end,int32_t x,int32_t y){
    end={};
    end.size=sizeof(end);
    end.version=TC_WIRE_ENDS_VERSION_1;
    end.x=x;end.y=y;
}
}
