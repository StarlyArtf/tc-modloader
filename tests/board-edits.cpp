#include "../src/board_edits.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static uint64_t readU64(const unsigned char* bytes,size_t offset){uint64_t value=0;std::memcpy(&value,bytes+offset,sizeof(value));return value;}

static_assert(sizeof(TCCommandV2)==80,"TCCommandV2 changed size; review the ABI baseline");
static_assert(offsetof(TCCommandV2,custom_prototype_id)==48,"TCCommandV2 payload offset changed");

int main(){
 using namespace tc::board_edits;
 const uint64_t customId=0x414E44325F303031ull;
 unsigned char record[kComponentRecordSize];
 std::memset(record,0xAB,sizeof(record));
 buildPlacement(kCustomKind,customId,30,0,0,record);

 /* The header the menu helper reads. */
 require(record[0]==kCustomKind,"the record kind is wrong");
 int32_t point=0;std::memcpy(&point,record+2,sizeof(point));
 require(point==packPoint(30,0),"the packed point is wrong");
 require(packPoint(-13,-7)==static_cast<int32_t>(0xFFF9FFF3u),"negative coordinates are packed incorrectly");
 require(record[6]==0,"the rotation byte is wrong");
 require(readU64(record,0x188)==customId,"the custom prototype id is wrong");
 /* The container defaults the menu template writes. */
 require(readU64(record,0x58)==1&&readU64(record,0x60)==kContainerCapacity&&record[0x68]==1,"container one defaults are wrong");
 require(readU64(record,0x70)==1&&readU64(record,0x78)==kContainerCapacity&&record[0x80]==1,"container two defaults are wrong");
 /* Everything else must be zero: a placement record with leftovers would hand
      the game fields the caller never set. */
 size_t nonZero=0;
 for(size_t i=0;i<kComponentRecordSize;++i){
  const bool known=(i==0)||(i>=2&&i<=5)||(i==6)||(i>=0x58&&i<0x82)||(i>=0x188&&i<0x190);
  if(record[i]&&!known)++nonZero;
 }
 require(nonZero==0,"a placement record carried bytes outside the template");
 /* A built-in placement keeps its own kind and no custom id. */
 TCCommandV2 builtin{};
 builtin.kind=0x04;
 require(placementKind(builtin)==0x04,"a built-in placement took the custom kind");
 TCCommandV2 custom{};
 custom.kind=kCustomKind;
 custom.custom_prototype_id=customId;
 require(placementKind(custom)==kCustomKind,"a custom placement did not mark the custom kind");
 buildPlacement(0x04,0,-32768,32767,255,record);
 require(record[0]==0x04,"a built-in record kept the custom kind");
 require(readU64(record,0x188)==0,"a built-in record kept a custom id");
 std::memcpy(&point,record+2,sizeof(point));
 require(point==packPoint(-32768,32767),"the extreme grid point is packed incorrectly");
 require(record[6]==255,"the rotation byte did not survive");
 std::cout<<"PASS board edits: placement record template and command payload mapping\n";
}
