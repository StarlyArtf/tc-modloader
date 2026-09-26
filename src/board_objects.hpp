#pragma once
#include "../sdk/tc_service_api.h"
#include <cstring>
#include <windows.h>

namespace tc::board_objects {
/* The pinned build's Board object tables, exactly as the loader's lifecycle
   fingerprint and the existing probes already read them:

     board + 0x78 / + 0x80   component sequence length / payload, stride 0x238
     board + 0x98 / + 0xa0   wire sequence length / payload, stride 0x68

   A sequence payload carries an 8-byte header, so element i lives at
   payload + 8 + i * stride.

   Component records: kind +0x00, schematic grid position +0x02/+0x04,
   direction byte +0x06, the component's own id +0x08 and the custom prototype
   id +0x188.  Sources: tests/component-placement-probe.cpp (writes that header
   back and finds the element), src/component_timing.hpp and
   docs/research/component-pipeline.md section 8.

   Wire records hold BOTH endpoints as int16 pairs - x1/y1 at +0x18/+0x1a and
   x2/y2 at +0x1c/+0x1e - plus the bit width at +0x30 and the simulation state
   byte offset at +0x38; the game's own wire id is the sequence index.  The
   and_gate built-in solution reads as (-6,-1) -> (-13,-1), a horizontal wire
   between the fixture's pins, which is what settled the two-endpoint reading.
   Source: docs/research/waveform-handoff.md section 3.2 (verified on the and_gate
   built-in solution) and docs/sdk/simulation.md.

   Deliberately NOT published yet, because their evidence is still partial: the
   instance chain (+0x10/+0x18 on a component) and the prototype pin
   descriptors.  Guessing them would be exactly what the handle design refuses
   to do. */
inline constexpr uint64_t kComponentStride=0x238,kWireStride=0x68,kRecordHeader=8;
inline constexpr uint64_t kMaxComponents=1000000,kMaxWires=4000000,kMaxStateOffset=0x2000000;
inline constexpr uint32_t kCustomComponentKind=0x4e,kMaxWireBits=64;
struct Arrays{uint64_t components=0,wires=0;const unsigned char* componentData=nullptr;const unsigned char* wireData=nullptr;};

inline uint8_t readU8(const unsigned char* bytes,size_t offset){return bytes[offset];}
inline uint32_t readU32(const unsigned char* bytes,size_t offset){uint32_t value=0;std::memcpy(&value,bytes+offset,sizeof(value));return value;}
inline uint64_t readU64(const unsigned char* bytes,size_t offset){uint64_t value=0;std::memcpy(&value,bytes+offset,sizeof(value));return value;}
inline int16_t readI16(const unsigned char* bytes,size_t offset){int16_t value=0;std::memcpy(&value,bytes+offset,sizeof(value));return value;}
inline const unsigned char* readPointer(const unsigned char* bytes,size_t offset){const unsigned char* value=nullptr;std::memcpy(&value,bytes+offset,sizeof(value));return value;}
/* True when [address, address+bytes) lies inside one committed, readable
   region.  Any pointer that comes out of a record is checked with this before
   it is followed, because the table a record names can be freed by an edit
   between two calls. */
inline bool readableRegion(const void* address,size_t bytes){
    if(!address||!bytes)return false;
    MEMORY_BASIC_INFORMATION region{};
    if(VirtualQuery(address,&region,sizeof(region))!=sizeof(region))return false;
    if(region.State!=MEM_COMMIT||(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const auto* begin=static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset=static_cast<size_t>(static_cast<const unsigned char*>(address)-begin);
    return offset+bytes<=region.RegionSize;
}

/* False when the Board has no readable object tables, or when the counts are
   outside the range the rest of the loader already treats as insane. */
inline bool readArrays(const void* board,Arrays& out){
    out=Arrays{};
    if(!board)return false;
    const auto* bytes=static_cast<const unsigned char*>(board);
    out.components=readU64(bytes,0x78);out.componentData=readPointer(bytes,0x80);
    out.wires=readU64(bytes,0x98);out.wireData=readPointer(bytes,0xa0);
    if(out.components>kMaxComponents||out.wires>kMaxWires)return false;
    if((out.components&&!out.componentData)||(out.wires&&!out.wireData))return false;
    return true;
}
/* A record pointer is only trusted when it sits exactly on an element of the
   Board's current sequence.  This is what turns a handle that outlived a board
   edit into STALE instead of a read of freed or reused memory. */
inline bool containsComponent(const Arrays& arrays,const void* record){
    if(!record||!arrays.componentData)return false;
    const auto* base=arrays.componentData+kRecordHeader;
    const auto* address=static_cast<const unsigned char*>(record);
    if(address<base)return false;
    const uint64_t delta=static_cast<uint64_t>(address-base);
    return delta%kComponentStride==0&&delta/kComponentStride<arrays.components;
}
inline bool containsWire(const Arrays& arrays,const void* record){
    if(!record||!arrays.wireData)return false;
    const auto* base=arrays.wireData+kRecordHeader;
    const auto* address=static_cast<const unsigned char*>(record);
    if(address<base)return false;
    const uint64_t delta=static_cast<uint64_t>(address-base);
    return delta%kWireStride==0&&delta/kWireStride<arrays.wires;
}
inline uint64_t componentIndex(const Arrays& arrays,const void* record){
    return (static_cast<uint64_t>(static_cast<const unsigned char*>(record)-(arrays.componentData+kRecordHeader)))/kComponentStride;
}
inline uint64_t wireIndex(const Arrays& arrays,const void* record){
    return (static_cast<uint64_t>(static_cast<const unsigned char*>(record)-(arrays.wireData+kRecordHeader)))/kWireStride;
}
inline void decodeComponent(const void* record,const TCGameHandle& handle,TCComponentInfoV1* out){
    const auto* bytes=static_cast<const unsigned char*>(record);
    out->size=sizeof(*out);out->version=TC_COMPONENT_INFO_VERSION_1;out->flags=0u;out->handle=handle;
    out->kind=readU8(bytes,0);out->x=readI16(bytes,2);out->y=readI16(bytes,4);
    out->rotation=readU8(bytes,6);out->id=readU64(bytes,8);
    out->custom_prototype_id=readU64(bytes,0x188);
    out->reserved0=0u;out->reserved1=0u;out->reserved2=0u;
    if(out->kind==kCustomComponentKind)out->flags|=TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE;
    else out->custom_prototype_id=0;
}
inline void decodeWire(const void* record,uint64_t index,const TCGameHandle& handle,TCWireInfoV1* out){
    const auto* bytes=static_cast<const unsigned char*>(record);
    out->size=sizeof(*out);out->version=TC_WIRE_INFO_VERSION_1;out->flags=TC_WIRE_INFO_HAS_ENDPOINT;out->handle=handle;
    out->id=index;
    out->x1=readI16(bytes,0x18);out->y1=readI16(bytes,0x1a);
    out->x2=readI16(bytes,0x1c);out->y2=readI16(bytes,0x1e);
    out->bit_width=0u;out->reserved0=0u;out->reserved1=0u;out->reserved2=0u;out->state_byte_offset=0;
    /* The width field is only verified on 1-bit nets so far, so a value outside
       1..64 is reported as "not usable" instead of as a width. */
    const uint32_t width=readU32(bytes,0x30);
    if(width>=1u&&width<=kMaxWireBits){out->bit_width=width;out->flags|=TC_WIRE_INFO_HAS_WIDTH;}
    const uint64_t slot=readU64(bytes,0x38);
    if(slot<kMaxStateOffset){out->state_byte_offset=slot;out->flags|=TC_WIRE_INFO_HAS_STATE_SLOT;}
}
}
