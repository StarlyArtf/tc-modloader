#pragma once
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
// Development probe for the audited game 2.1.334 ABI. The game jit routine
// takes a Nim byte sequence (length, payload; payload starts after capacity)
// and a byte offset. Captures happen before execution, without rewriting code.
namespace jit_code_dump {
inline const TCHost* host=nullptr;
inline void (*original)(void*,int64_t)=nullptr;
inline std::atomic<unsigned> serial{0};
inline void capture(void* sequence,int64_t entry) {
    int64_t length=0;void* payload=nullptr;
    std::memcpy(&length,sequence,8);
    std::memcpy(&payload,static_cast<char*>(sequence)+8,8);
    const unsigned number=serial.fetch_add(1);
    if(number<16 && payload && length>0 && length<16000000 && entry>=0 && entry<length) {
        const std::string path=std::string(host->data_directory_utf8)+"/jit-code-"+std::to_string(number)+".bin";
        std::ofstream file(path,std::ios::binary);
        file.write(static_cast<char*>(payload)+8,length);
        const std::string message="jit-dump: file="+std::to_string(number)+" bytes="+std::to_string(length)+" entry="+std::to_string(entry)+" thread="+std::to_string(GetCurrentThreadId());
        host->log(host->context,message.c_str());
    }
    original(sequence,entry);
}
inline int install(const TCHost* h) {
    const char* enabled=std::getenv("TC_TEST_JIT_DUMP");
    if(!enabled || enabled[0]!='1')return 0;
    if(!h->game_build || std::strcmp(h->game_build,"2.1.334 / native-api-1")!=0)return 1;
    host=h;
    void* target=h->resolve_symbol(h->context,"jit__modelZsimulationZjitZjit_u1807");
    if(!target)return 1;
    return h->create_hook(h->context,target,reinterpret_cast<void*>(&capture),reinterpret_cast<void**>(&original));
}
}
