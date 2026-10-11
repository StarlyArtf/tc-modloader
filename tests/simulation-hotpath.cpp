// Generated-code boundary checks: idle capture and editable constants must not
// pay for a foreign call. The game playtests exercise the actual JIT loads.
#include "../vendor/minhook/include/MinHook.h"
#include "../src/native_logic.hpp"
#include <cassert>
#include <iostream>

namespace {
std::string generated;
void collect(const tc::TCNimString* line,void*) {
    generated.assign(static_cast<const char*>(line->data)+8,line->length);
}
void emit(const std::string& text) {
    std::vector<char> bytes(8+text.size()+1);
    std::memcpy(bytes.data()+8,text.data(),text.size());
    tc::TCNimString line{uint64_t(text.size()),bytes.data()};
    tc::logic::line(&line,nullptr);
}
}

int main() {
    using namespace tc::logic;
    originalLine=&collect;
    constexpr uint64_t id=4805296447517530978ULL;
    for(uint32_t width:{16u,32u,64u})for(bool refresh:{false,true}) {
        Emission context;
        context.constants[89]={id,10,width};
        emission=&context;
        setDynamicConstant(id,10);
        emit("// 89 com_constant");
        emit(refresh?"let value_id316 = ( 10 & 0xffffffffffffffff)":
                     "var vid316 = U64 ( 10 & 0xffffffffffffffff)");
        assert(context.rewritten==1);
        const std::string prefix="load(<U64>, Ptr ";
        const auto at=generated.find(prefix);
        assert(at!=std::string::npos && "constants still cross the DLL every cycle");
        assert(generated.find("game_engine.")==std::string::npos);
        const auto address=std::stoull(generated.substr(at+prefix.size()));
        auto* slot=reinterpret_cast<std::atomic<uint64_t>*>(static_cast<uintptr_t>(address));
        assert(slot->load(std::memory_order_relaxed)==10);
        setDynamicConstant(id,0xfedcba9876543210ULL);
        assert(slot->load(std::memory_order_relaxed)==0xfedcba9876543210ULL);
        // Registry growth cannot invalidate pointers baked into a JIT program.
        for(uint64_t other=10000;other<14000;++other)setDynamicConstant(other,other);
        assert(slot==dynamicConstantSlot(id,0));
        assert(slot->load(std::memory_order_relaxed)==0xfedcba9876543210ULL);
        emission=nullptr;
    }
    std::cout<<"PASS simulation hot path: 16/32/64-bit live constants, both phases, stable JIT addresses\n";
}
