#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace tc::component_geometry {

inline constexpr size_t kShapeSequenceOffset=0x48;
inline constexpr size_t kShapeStorageOffset=0x50;
inline constexpr size_t kShapeRadiusOffset=0x58;

struct Rectangle {
    int16_t x=0,y=0;
    uint16_t width=0,height=0,radius=0;
};

/* The pin lane of a generated component.  Its pins sit on one vertical lane per
   side - at (2,0), (2,-1), ... for a default type, at (3,0), (3,-1), ... for a
   type that declared 3.0 (measured: enlarging the footprint never moves them,
   while the declared lane does) - and the footprint is the box the game
   hit-tests, selects, drags by and reserves.  A footprint that reaches past
   that lane therefore swallows the pins: the wire can no longer be started by
   dragging from the pin, and the reserved space extends past it.

   This has bitten the same way twice in this repository: the interactive pair
   chose 2.0 half-width for exactly this reason (examples/clock/plugin.cpp, "the
   stock output pin is at local (2,0)"), and the clock's first CONST-sized layout
   declared 3.5 and lost the pin until it was found again.  The warning below is
   what makes the third time cheap: the number is not a matter of taste, it is
   where the pins are - and it is per type, because a type may now ask for a
   wider lane than the default.  The height is free - only the width is pinned
   here. */
inline constexpr float kPinLaneX=2.0f;
inline bool footprintReachesPins(float halfWidth,bool hasPins,float pinLane=kPinLaneX) {
    if(!hasPins)return false;
    const float lane=std::isfinite(pinLane)&&pinLane>=0.5f?pinLane:kPinLaneX;
    return halfWidth>lane;
}

/* The game stores integer grid rectangles.  The public half extents are
   rounded outwards, so a declaration can never occupy less space than asked. */
inline bool fromHalfExtents(float halfWidth,float halfHeight,Rectangle& out){
    if(!std::isfinite(halfWidth)||!std::isfinite(halfHeight)||
       halfWidth<0.5f||halfHeight<0.5f)return false;
    const double wide=std::ceil(static_cast<double>(halfWidth)*2.0);
    const double high=std::ceil(static_cast<double>(halfHeight)*2.0);
    if(wide>32767.0||high>32767.0)return false;
    out.width=static_cast<uint16_t>(wide);out.height=static_cast<uint16_t>(high);
    out.x=static_cast<int16_t>(-static_cast<int>(out.width)/2);
    out.y=static_cast<int16_t>(-static_cast<int>(out.height)/2);
    const int left=std::abs(static_cast<int>(out.x));
    const int right=std::abs(static_cast<int>(out.x)+static_cast<int>(out.width)-1);
    const int top=std::abs(static_cast<int>(out.y));
    const int bottom=std::abs(static_cast<int>(out.y)+static_cast<int>(out.height)-1);
    const int radius=(left>right?left:right)+(top>bottom?top:bottom);
    if(radius>std::numeric_limits<uint16_t>::max())return false;
    out.radius=static_cast<uint16_t>(radius);
    return true;
}

inline uint64_t pack(const Rectangle& value){
    return uint64_t(uint16_t(value.x))|
           (uint64_t(uint16_t(value.y))<<16)|
           (uint64_t(value.width)<<32)|
           (uint64_t(value.height)<<48);
}

inline Rectangle unpack(uint64_t value){
    Rectangle out{};
    out.x=static_cast<int16_t>(value&0xffffu);
    out.y=static_cast<int16_t>((value>>16)&0xffffu);
    out.width=static_cast<uint16_t>((value>>32)&0xffffu);
    out.height=static_cast<uint16_t>((value>>48)&0xffffu);
    return out;
}

inline void halfExtents(const Rectangle& value,uint8_t rotation,float& width,float& height){
    width=static_cast<float>(value.width)*0.5f;
    height=static_cast<float>(value.height)*0.5f;
    if(rotation&1u){const float swap=width;width=height;height=swap;}
}

} // namespace tc::component_geometry
