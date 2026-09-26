#pragma once
#include "../sdk/tc_command_api.h"
#include <cstring>

namespace tc::board_edits {
/* A board component record is 0x238 bytes; the placement template below is the
   one the game's own component menu builds.  Every field was verified by
   tests/component-placement-probe.cpp, which fills the same template, hands it
   to the menu helper and then finds the new component in the board's sequence:

     +0x00  u8    kind (0x4e for a custom instance)
     +0x02  i32   point: low 16 bits x, high 16 bits y
     +0x06  u8    rotation/direction
     +0x58 / +0x60  container one: count / capacity
     +0x68          container one: occupied flag
     +0x70 / +0x78  container two: count / capacity
     +0x80          container two: occupied flag
     +0x188 u64   custom prototype ID

   See docs/research/component-pipeline.md section 8 for the field table. */
inline constexpr uint64_t kComponentRecordSize=0x238;
inline constexpr uint64_t kContainerCapacity=0x100;
inline constexpr uint32_t kCustomKind=0x4e;

inline int32_t packPoint(int16_t x,int16_t y){
    return static_cast<int32_t>((static_cast<uint32_t>(y)<<16)|static_cast<uint16_t>(x));
}
inline void writeU64(unsigned char* bytes,size_t offset,uint64_t value){
    std::memcpy(bytes+offset,&value,sizeof(value));
}
/* Fills `record` (kComponentRecordSize bytes) with a placement the game's add
   helper accepts.  `kind` is the built-in kind unless `customPrototypeId` is
   non-zero, in which case the record is a custom instance and the kind must be
   kCustomKind. */
inline void buildPlacement(uint32_t kind,uint64_t customPrototypeId,int16_t x,int16_t y,
                           uint32_t rotation,unsigned char* record){
    std::memset(record,0,kComponentRecordSize);
    record[0]=static_cast<unsigned char>(kind);
    const int32_t point=packPoint(x,y);
    std::memcpy(record+2,&point,sizeof(point));
    record[6]=static_cast<unsigned char>(rotation);
    writeU64(record,0x58,1);
    writeU64(record,0x60,kContainerCapacity);
    record[0x68]=1;
    writeU64(record,0x70,1);
    writeU64(record,0x78,kContainerCapacity);
    record[0x80]=1;
    writeU64(record,0x188,customPrototypeId);
}
/* The command form decides which kind the record carries. */
inline uint32_t placementKind(const TCCommandV2& command){
    return command.custom_prototype_id?kCustomKind:command.kind;
}
}
