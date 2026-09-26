#include "../src/component_geometry.hpp"
#include "../sdk/tc_service_api.h"
#include <cassert>
#include <cstddef>
#include <cmath>
#include <iostream>

static_assert(offsetof(TCComponentGeometryApiV2,set_instance_footprint)==
              sizeof(TCComponentGeometryApiV1));
static_assert(sizeof(TCComponentGeometryApiV2)==
              sizeof(TCComponentGeometryApiV1)+sizeof(void*));
/* V3 keeps the complete V2 prefix and appends the cell form: a caller compiled
   against V2 keeps working, and a caller that wants the offset a multi-pin face
   has asks for the cells directly. */
static_assert(offsetof(TCComponentGeometryApiV3,set_footprint_cells)==
              sizeof(TCComponentGeometryApiV2));
static_assert(sizeof(TCComponentGeometryApiV3)==
              sizeof(TCComponentGeometryApiV2)+sizeof(void*));

int main(){
    using namespace tc::component_geometry;
    Rectangle value{};
    assert(!fromHalfExtents(0.49f,1.f,value));
    assert(!fromHalfExtents(NAN,1.f,value));
    assert(!fromHalfExtents(20000.f,1.f,value));
    assert(fromHalfExtents(6.f,3.f,value));
    assert(value.x==-6&&value.y==-3&&value.width==12&&value.height==6&&value.radius==9);
    const auto roundTrip=unpack(pack(value));
    assert(roundTrip.x==-6&&roundTrip.y==-3&&roundTrip.width==12&&roundTrip.height==6);
    assert(fromHalfExtents(1.1f,2.1f,value));
    assert(value.width==3&&value.height==5&&value.x==-1&&value.y==-2);
    float width=0,height=0;
    halfExtents(value,0,width,height);assert(width==1.5f&&height==2.5f);
    halfExtents(value,1,width,height);assert(width==2.5f&&height==1.5f);
    /* The cell form (V3) stores the rectangle as asked, offset included: the
       face of a type whose pins start at row 0 sits below the component's own
       cell, and the box has to follow it there (`tests/float-pitch-probe.cpp`
       measures what a centred box costs). */
    Rectangle cells{};
    cells.x=0;cells.y=0;cells.width=5;cells.height=4;cells.radius=4;
    const auto cellTrip=unpack(pack(cells));
    assert(cellTrip.x==0&&cellTrip.y==0&&cellTrip.width==5&&cellTrip.height==4);
    halfExtents(cellTrip,0,width,height);assert(width==2.5f&&height==2.0f);
    halfExtents(cellTrip,1,width,height);assert(width==2.0f&&height==2.5f);
    /* The pin lane: a type with pins must keep its footprint inside it, a
       decorative type is free (see the comment on kPinLaneX).  The lane is per
       type since the pin-lane cut: the default is 2.0, and a type that asked
       for a wider one measures against its own number. */
    assert(!footprintReachesPins(2.0f,true));
    assert(!footprintReachesPins(1.5f,true));
    assert(footprintReachesPins(3.5f,true));
    assert(footprintReachesPins(2.001f,true));
    assert(!footprintReachesPins(4.0f,false));
    assert(!footprintReachesPins(2.5f,true,3.0f));
    assert(footprintReachesPins(3.5f,true,3.0f));
    assert(footprintReachesPins(2.5f,true,2.0f));
    assert(!footprintReachesPins(4.0f,false,3.0f));
    /* A nonsense lane falls back to the default instead of swallowing the
       pins' area silently. */
    assert(footprintReachesPins(2.5f,true,0.f));
    std::cout<<"component geometry tests passed\n";
}
