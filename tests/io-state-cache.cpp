#include "../src/io_state_cache.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

static void require(bool value,const char* message){
 if(!value){std::cerr<<"FAIL: "<<message<<"\n";std::exit(1);}
}

int main(){
 alignas(std::uintptr_t) std::array<unsigned char,0xdb00> context{};
 bool generated=false,destroyed=false;
 int copies=0;
 const bool refreshed=tc::io_state_cache::refresh(
  reinterpret_cast<void*>(1),context.data(),
  [&](void*,tc::io_state_cache::Groups* groups){
   generated=true;
   groups->words={2,0x1111,3,0x2222,4,0x3333};
  },
  [&](void* destination,const std::uintptr_t* source){
   ++copies;
   auto* words=static_cast<std::uintptr_t*>(destination);
   words[0]=source[0];words[1]=source[1];
  },
  [&](tc::io_state_cache::Groups*){destroyed=true;});

 const auto pair_at=[&](std::size_t offset){
  return reinterpret_cast<const std::uintptr_t*>(context.data()+offset);
 };
 require(refreshed&&generated,"refresh generates fresh IO states");
 require(copies==3,"all three IO-state groups are copied");
 require(pair_at(tc::io_state_cache::input_offset)[0]==2&&
         pair_at(tc::io_state_cache::input_offset)[1]==0x1111,
         "input labels replace the input cache");
 require(pair_at(tc::io_state_cache::output_offset)[0]==3&&
         pair_at(tc::io_state_cache::output_offset)[1]==0x2222,
         "output labels replace the output cache");
 require(pair_at(tc::io_state_cache::other_offset)[0]==4&&
         pair_at(tc::io_state_cache::other_offset)[1]==0x3333,
         "the remaining IO group is preserved by the rebuild");
 require(destroyed,"temporary managed sequences are destroyed");
 require(!tc::io_state_cache::refresh(nullptr,context.data(),
          [](void*,void*){},[](void*,void*){},[](void*){}),
         "an invalid board is ignored");
 std::cout<<"PASS io-state cache: input/output labels are rebuilt together\n";
}
