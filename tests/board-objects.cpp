#include "../src/board_objects.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void putU8(unsigned char* bytes,size_t offset,uint8_t value){bytes[offset]=value;}
static void putI16(unsigned char* bytes,size_t offset,int16_t value){std::memcpy(bytes+offset,&value,sizeof(value));}
static void putU32(unsigned char* bytes,size_t offset,uint32_t value){std::memcpy(bytes+offset,&value,sizeof(value));}
static void putU64(unsigned char* bytes,size_t offset,uint64_t value){std::memcpy(bytes+offset,&value,sizeof(value));}
static void putPointer(unsigned char* bytes,size_t offset,const void* value){std::memcpy(bytes+offset,&value,sizeof(value));}

static_assert(sizeof(TCComponentInfoV1)==88,"TCComponentInfoV1 changed size; review the ABI baseline");
static_assert(sizeof(TCWireInfoV1)==88,"TCWireInfoV1 changed size; review the ABI baseline");
static_assert(offsetof(TCComponentInfoV1,handle)==16,"TCComponentInfoV1 handle offset changed");
static_assert(offsetof(TCWireInfoV1,state_byte_offset)==72,"TCWireInfoV1 state slot offset changed");

int main(){
 using namespace tc::board_objects;
 const uint64_t customId=0x414E44325F303031ull;
 alignas(8) unsigned char board[0x100]{};
 std::vector<unsigned char> componentPayload(kRecordHeader+2*kComponentStride,0);
 std::vector<unsigned char> wirePayload(kRecordHeader+3*kWireStride,0);
 unsigned char* const component0=componentPayload.data()+kRecordHeader;
 unsigned char* const component1=component0+kComponentStride;
 unsigned char* const wire0=wirePayload.data()+kRecordHeader;
 unsigned char* const wire1=wire0+kWireStride;
 unsigned char* const wire2=wire1+kWireStride;

 /* A custom instance at (-7,12) and a built-in AND at (30,0). */
 putU8(component0,0,0x4e);putI16(component0,2,-7);putI16(component0,4,12);putU8(component0,6,3);
 putU64(component0,8,0x1111);putU64(component0,0x188,customId);
 putU8(component1,0,0x04);putI16(component1,2,30);putI16(component1,4,0);putU8(component1,6,0);
 putU64(component1,8,0x2222);putU64(component1,0x188,0xdeadbeefull);
 /* Three wires: a 1-bit net, a bridge-slot word net and a record whose width and
    slot fields are outside anything the pinned build has produced so far. */
 putI16(wire0,0x18,-4);putI16(wire0,0x1a,-1);putI16(wire0,0x1c,5);putI16(wire0,0x1e,-1);putU32(wire0,0x30,1);putU64(wire0,0x38,256);
 putI16(wire1,0x18,0);putI16(wire1,0x1a,0);putI16(wire1,0x1c,0);putI16(wire1,0x1e,4);putU32(wire1,0x30,8);putU64(wire1,0x38,0x9a0000);
 putI16(wire2,0x18,32767);putI16(wire2,0x1a,-32768);putI16(wire2,0x1c,7);putI16(wire2,0x1e,-9);putU32(wire2,0x30,65);putU64(wire2,0x38,0x4000000);
 putU64(board,0x78,2);putPointer(board,0x80,componentPayload.data());
 putU64(board,0x98,3);putPointer(board,0xa0,wirePayload.data());

 Arrays arrays{};
 require(readArrays(board,arrays),"a well-formed board was rejected");
 require(arrays.components==2&&arrays.wires==3,"board counts were read incorrectly");
 require(arrays.componentData==componentPayload.data()&&arrays.wireData==wirePayload.data(),"board payloads were read incorrectly");
 Arrays rejected{};
 require(!readArrays(nullptr,rejected),"a null board was accepted");
 alignas(8) unsigned char insane[0x100]{};putU64(insane,0x78,kMaxComponents+1);putPointer(insane,0x80,componentPayload.data());
 require(!readArrays(insane,rejected),"an insane component count was accepted");
 alignas(8) unsigned char missing[0x100]{};putU64(missing,0x78,1);
 require(!readArrays(missing,rejected),"a missing payload was accepted");

 require(containsComponent(arrays,component0)&&containsComponent(arrays,component1),"a live component was refused");
 require(componentIndex(arrays,component1)==1,"a component index was computed incorrectly");
 require(!containsComponent(arrays,componentPayload.data()),"the sequence header was accepted as an element");
 require(!containsComponent(arrays,component1+kComponentStride),"an element past the end was accepted");
 require(!containsComponent(arrays,component1+1),"a misaligned element was accepted");
 require(!containsComponent(arrays,wire0),"a wire was accepted as a component");
 require(containsWire(arrays,wire0)&&containsWire(arrays,wire2),"a live wire was refused");
 require(wireIndex(arrays,wire2)==2,"a wire index was computed incorrectly");
 require(!containsWire(arrays,wire2+kWireStride),"a wire past the end was accepted");
 require(!containsWire(arrays,component0),"a component was accepted as a wire");

 TCGameHandle handle{sizeof(TCGameHandle),TC_GAME_OBJECT_COMPONENT,7,9};
 TCComponentInfoV1 component{};
 decodeComponent(component0,handle,&component);
 require(component.size==sizeof(component)&&component.version==TC_COMPONENT_INFO_VERSION_1,"bad component header");
 require(component.handle.generation==handle.generation&&component.handle.token==handle.token,"component handle was not echoed");
 require(component.kind==0x4e&&component.x==-7&&component.y==12&&component.rotation==3,"component header fields are wrong");
 require(component.id==0x1111,"component id is wrong");
 require((component.flags&TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE)&&component.custom_prototype_id==customId,"custom prototype was not reported");
 decodeComponent(component1,handle,&component);
 require(component.kind==0x04&&component.x==30&&component.y==0&&component.id==0x2222,"built-in component fields are wrong");
 require(!(component.flags&TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE)&&component.custom_prototype_id==0,"a built-in component kept a prototype id");

 TCGameHandle wireHandle{sizeof(TCGameHandle),TC_GAME_OBJECT_WIRE,7,11};
 TCWireInfoV1 wire{};
 decodeWire(wire0,0,wireHandle,&wire);
 require(wire.size==sizeof(wire)&&wire.version==TC_WIRE_INFO_VERSION_1,"bad wire header");
 require(wire.id==0&&wire.x1==-4&&wire.y1==-1&&wire.x2==5&&wire.y2==-1,"wire endpoint is wrong");
 require((wire.flags&TC_WIRE_INFO_HAS_ENDPOINT),"wire endpoint flag missing");
 require((wire.flags&TC_WIRE_INFO_HAS_WIDTH)&&wire.bit_width==1,"wire width is wrong");
 require((wire.flags&TC_WIRE_INFO_HAS_STATE_SLOT)&&wire.state_byte_offset==256,"wire state slot is wrong");
 decodeWire(wire1,1,wireHandle,&wire);
 require(wire.id==1&&wire.bit_width==8&&wire.state_byte_offset==0x9a0000,"a bridge-slot wire was reported incorrectly");
 decodeWire(wire2,2,wireHandle,&wire);
 require(wire.id==2&&wire.x1==32767&&wire.y1==-32768&&wire.x2==7&&wire.y2==-9,"wire coordinates were not sign extended");
 require(!(wire.flags&TC_WIRE_INFO_HAS_WIDTH)&&wire.bit_width==0,"an out-of-range width was published");
 require(!(wire.flags&TC_WIRE_INFO_HAS_STATE_SLOT)&&wire.state_byte_offset==0,"an out-of-range state slot was published");
 std::cout<<"PASS board objects: sequences, element membership, component and wire decoding\n";
}
