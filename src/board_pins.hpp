#pragma once
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_service_api.h"
#include <cstring>
#include <set>

namespace tc::board_pins {
/* The built-in prototype table is the two-qword object the SDK's TCGameModel
   walks: length, bucket pointer.  Buckets are 0x5b8 bytes, an occupied one has
   a nonzero hash at +0x08 and holds its single-byte kind at +0x10.

   Enumerating it is the only safe way to learn which kinds may be handed to the
   game's get_prototype(): an unknown key raises a Nim error that a plugin cannot
   recover from, so the host builds this set once at boot and refuses anything
   outside it. */
inline constexpr uint64_t kBucketStride=0x5b8,kMaxBuckets=4096;
inline constexpr size_t kBucketHashOffset=0x08,kBucketKeyOffset=0x10;
inline constexpr uint64_t kPinAutoSize=0x7fffffffffffffffULL;

inline bool readU64(const void* address,size_t offset,uint64_t& value){
    if(!address)return false;
    std::memcpy(&value,static_cast<const unsigned char*>(address)+offset,sizeof(value));
    return true;
}
inline bool readPointer(const void* address,size_t offset,const void*& value){
    if(!address)return false;
    std::memcpy(&value,static_cast<const unsigned char*>(address)+offset,sizeof(value));
    return true;
}
/* False when the table itself is missing or implausible. */
inline bool tableShape(const void* table,uint64_t& buckets,const unsigned char*& data){
    if(!table)return false;
    const void* bucketsPointer=nullptr;
    if(!readU64(table,0,buckets)||!readPointer(table,8,bucketsPointer))return false;
    /* Length zero is legitimate; anything above the guard is a layout mismatch. */
    if(buckets>kMaxBuckets)return false;
    if(buckets&&!bucketsPointer)return false;
    data=static_cast<const unsigned char*>(bucketsPointer);
    return true;
}
inline bool builtinKindAt(const void* table,uint64_t index,uint8_t& kind){
    uint64_t length=0;const unsigned char* buckets=nullptr;
    if(!tableShape(table,length,buckets))return false;
    uint64_t seen=0;
    for(uint64_t i=0;i<length;++i){
        const auto* bucket=buckets+i*kBucketStride;
        uint64_t hash=0;
        std::memcpy(&hash,bucket+kBucketHashOffset,sizeof(hash));
        if(!hash)continue;
        if(seen==index){kind=bucket[kBucketKeyOffset];return true;}
        ++seen;
    }
    return false;
}
inline void collectBuiltinKinds(const void* table,std::set<uint8_t>& out){
    out.clear();
    uint64_t length=0;const unsigned char* buckets=nullptr;
    if(!tableShape(table,length,buckets))return;
    for(uint64_t i=0;i<length;++i){
        const auto* bucket=buckets+i*kBucketStride;
        uint64_t hash=0;
        std::memcpy(&hash,bucket+kBucketHashOffset,sizeof(hash));
        if(hash)out.insert(bucket[kBucketKeyOffset]);
    }
}
/* Copies the two fields the SDK verified on the real engine: the relative point
   inside the descriptor that starts at entry+8, and the raw WordSize at
   entry+0x10 (docs/research/board-object-fields.md section 2). */
inline TCPinInfoV1 decodePin(const TCPin& pin,uint32_t direction){
    TCPinInfoV1 info{};
    info.size=sizeof(info);
    info.version=TC_COMPONENT_PINS_VERSION_1;
    info.direction=direction;
    info.flags=TC_PIN_INFO_HAS_POSITION;
    int16_t x=0,y=0;
    std::memcpy(&x,pin.bytes+8+2,sizeof(x));
    std::memcpy(&y,pin.bytes+8+4,sizeof(y));
    info.x=x;info.y=y;
    std::memcpy(&info.word_size_raw,pin.bytes+0x10,sizeof(info.word_size_raw));
    if(info.word_size_raw>=1&&info.word_size_raw<=64)info.bits=static_cast<uint32_t>(info.word_size_raw);
    else if(info.word_size_raw==kPinAutoSize)info.flags|=TC_PIN_INFO_WIDTH_AUTO;
    info.reserved0=0u;
    return info;
}
}
