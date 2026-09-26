#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
namespace tc::snappy_raw {
/* The raw Snappy block a circuit.data stores after its version byte.  Only what
   the dependency scan needs: a bounded, total decoder with no encoder (the
   fixtures have a literal-only encoder of their own, tests/*-fixture.cpp). */
inline bool varint(const unsigned char* data,size_t size,size_t& pos,uint64_t& value){
 uint64_t result=0;int shift=0;
 while(pos<size){
  const unsigned char byte=data[pos++];
  result|=(uint64_t)(byte&0x7f)<<shift;
  if(!(byte&0x80)){value=result;return true;}
  shift+=7;
  if(shift>63)return false;
 }
 return false;
}
/* limit caps the *decompressed* size: a circuit that claims to expand past it is
   refused rather than allocated (a save file is player-supplied data). */
inline bool decode(const unsigned char* data,size_t size,size_t limit,
                   std::vector<unsigned char>& out){
 size_t pos=0;uint64_t expected=0;
 if(!varint(data,size,pos,expected)||expected>limit)return false;
 out.clear();out.reserve((size_t)expected);
 while(pos<size&&out.size()<expected){
  const unsigned char tag=data[pos++];
  const unsigned kind=tag&3;
  size_t length=0,offset=0;
  if(kind==0){
   length=(size_t)(tag>>2);
   if(length<60){length+=1;}
   else{
    const size_t extra=length-59;
    if(extra>4||pos+extra>size)return false;
    length=0;for(size_t i=0;i<extra;++i)length|=(size_t)data[pos++]<<(8*i);
    length+=1;
   }
   if(pos+length>size)return false;
   out.insert(out.end(),data+pos,data+pos+length);pos+=length;
   continue;
  }
  if(kind==1){
   length=((size_t)(tag>>2)&0x7)+4;
   if(pos>=size)return false;
   offset=((size_t)(tag>>5)<<8)|data[pos++];
  }else if(kind==2){
   length=(size_t)(tag>>2)+1;
   if(pos+2>size)return false;
   offset=(size_t)data[pos]|((size_t)data[pos+1]<<8);pos+=2;
  }else{
   length=(size_t)(tag>>2)+1;
   if(pos+4>size)return false;
   offset=(size_t)data[pos]|((size_t)data[pos+1]<<8)|((size_t)data[pos+2]<<16)|
          ((size_t)data[pos+3]<<24);
   pos+=4;
  }
  if(!offset||offset>out.size())return false;
  const size_t start=out.size()-offset;
  /* Copies in chunks instead of byte by byte: the buffer was reserved to the
     declared length, so appending from inside it cannot reallocate, and a
     circuit is mostly copies.  This is what the dependency scan spends its time
     in (125 ms for a 139 circuit profile before, measured on the render thread). */
  size_t produced=0;
  {
   const size_t room=std::min(length,(size_t)expected-out.size());
   while(produced<room){
    const size_t chunk=std::min(room-produced,offset);
    out.insert(out.end(),out.begin()+(start+produced),out.begin()+(start+produced+chunk));
    produced+=chunk;
   }
  }
 }
 return out.size()==(size_t)expected;
}
}
