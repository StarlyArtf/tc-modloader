#include "../src/board_pins.hpp"
#include "../src/board_connect.hpp"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void putU64(unsigned char* bytes,size_t offset,uint64_t value){std::memcpy(bytes+offset,&value,sizeof(value));}
static void putPointer(unsigned char* bytes,size_t offset,const void* value){std::memcpy(bytes+offset,&value,sizeof(value));}
static void putI16(unsigned char* bytes,size_t offset,int16_t value){std::memcpy(bytes+offset,&value,sizeof(value));}

static_assert(sizeof(TCPinInfoV1)==40,"TCPinInfoV1 changed size; review the ABI baseline");
static_assert(offsetof(TCPinInfoV1,word_size_raw)==32,"TCPinInfoV1 word size offset changed");

int main(){
 using namespace tc::board_pins;
 /* A PROTOTYPES-shaped table: length, bucket pointer, 0x5b8-byte buckets with a
    nonzero hash at +0x08 and the kind at +0x10.  One bucket is left empty so the
    "occupied" rule is covered, not just the index arithmetic. */
 const uint64_t bucketCount=5;
 std::vector<unsigned char> buckets(bucketCount*kBucketStride,0);
 const uint8_t kinds[]={0x03,0x04,0x44};
 for(uint64_t i=0;i<3;++i){
  putU64(buckets.data()+i*kBucketStride,kBucketHashOffset,i+1);
  buckets[i*kBucketStride+kBucketKeyOffset]=kinds[i];
 }
 unsigned char table[16]{};
 putU64(table,0,bucketCount);
 putPointer(table,8,buckets.data());

 std::set<uint8_t> collected;
 collectBuiltinKinds(table,collected);
 require(collected.size()==3,"the built-in kind set has the wrong size");
 require(collected.count(0x03)&&collected.count(0x04)&&collected.count(0x44),"the built-in kind set lost a kind");
 require(!collected.count(0x00)&&!collected.count(0x99),"an unoccupied bucket produced a kind");
 uint8_t kind=0xff;
 require(builtinKindAt(table,0,kind)&&kind==0x03,"occupied index 0 is wrong");
 require(builtinKindAt(table,2,kind)&&kind==0x44,"occupied index 2 is wrong");
 require(!builtinKindAt(table,3,kind),"an index past the occupied entries was accepted");
 /* Guards: a broken table must yield nothing instead of walking memory. */
 collectBuiltinKinds(nullptr,collected);
 require(collected.empty(),"a null table produced kinds");
 unsigned char huge[16]{};putU64(huge,0,kMaxBuckets+1);putPointer(huge,8,buckets.data());
 collectBuiltinKinds(huge,collected);
 require(collected.empty(),"an implausible table length produced kinds");
 unsigned char dangling[16]{};putU64(dangling,0,2);
 collectBuiltinKinds(dangling,collected);
 require(collected.empty(),"a missing bucket pointer produced kinds");
 unsigned char empty[16]{};
 collectBuiltinKinds(empty,collected);
 require(collected.empty(),"an empty table produced kinds");

 /* Pin entries: the point lives inside the descriptor that starts eight bytes
    into the entry, and the raw WordSize sits at entry+0x10. */
 tc::TCPin numeric{};
 putI16(numeric.bytes,8+2,-1);
 putI16(numeric.bytes,8+4,1);
 putU64(numeric.bytes,0x10,1);
 /* A decoy at the entry's own +2: a reader that forgets the anchor would take it. */
 putI16(numeric.bytes,2,77);
 putI16(numeric.bytes,4,88);
 const TCPinInfoV1 decoded=decodePin(numeric,TC_PIN_INPUT);
 require(decoded.size==sizeof(decoded)&&decoded.version==TC_COMPONENT_PINS_VERSION_1,"bad pin header");
 require(decoded.direction==TC_PIN_INPUT,"bad pin direction");
 require((decoded.flags&TC_PIN_INFO_HAS_POSITION)&&!(decoded.flags&TC_PIN_INFO_WIDTH_AUTO),"bad pin flags");
 require(decoded.x==-1&&decoded.y==1,"the pin point was read from the wrong anchor");
 require(decoded.bits==1&&decoded.word_size_raw==1,"the pin word size was read incorrectly");

 tc::TCPin autoSized{};
 putI16(autoSized.bytes,8+2,2);
 putI16(autoSized.bytes,8+4,0);
 putU64(autoSized.bytes,0x10,kPinAutoSize);
 const TCPinInfoV1 autoPin=decodePin(autoSized,TC_PIN_OUTPUT);
 require(autoPin.direction==TC_PIN_OUTPUT&&autoPin.x==2&&autoPin.y==0,"an output pin was decoded incorrectly");
 require((autoPin.flags&TC_PIN_INFO_WIDTH_AUTO)&&autoPin.bits==0,"AUTO_SIZE was not reported as a flag");
 require(autoPin.word_size_raw==kPinAutoSize,"the raw word size was not preserved");

 tc::TCPin weird{};
 putU64(weird.bytes,0x10,0x7ffffffffffffffeull);
 const TCPinInfoV1 weirdPin=decodePin(weird,TC_PIN_INPUT);
 require((weirdPin.flags&TC_PIN_INFO_WIDTH_AUTO)==0&&weirdPin.bits==0,"a non-numeric width was published as bits");
 /* The connection rule the campaign level measured: the wire end (9,0) is the
    output pin's input port (10,0)+(-1,0), and (-9,0) is the input pin's output
    port (-10,0)+(1,0). */
 require(tc::board_connect::pinSitsOnPoint(9,0,10,0,-1,0),"the output pin's port was not matched");
 require(tc::board_connect::pinSitsOnPoint(-9,0,-10,0,1,0),"the input pin's port was not matched");
 require(!tc::board_connect::pinSitsOnPoint(9,0,10,0,1,0),"a wrong port offset was accepted");
 require(!tc::board_connect::pinSitsOnPoint(-9,1,-10,0,1,0),"a wire end off the port was accepted");
 require(tc::board_connect::withinWindow(9,0,10,0)&&tc::board_connect::withinWindow(-9,0,-10,0),"a nearby component was filtered out");
 require(!tc::board_connect::withinWindow(9,0,10+tc::board_connect::kSearchWindow+1,0),"a far component was not filtered out");
 std::cout<<"PASS board pins: built-in kind enumeration guards and pin decode\n";
}
