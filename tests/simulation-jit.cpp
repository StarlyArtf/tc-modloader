#include "../src/simulation_jit.hpp"
#include <windows.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iterator>
#define CHECK(x) do {if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": " #x "\n";return 1;}}while(false)
using Function=uint64_t(*)(uint64_t,uint64_t*,uint64_t);
struct Executable {
    void* memory=nullptr;Function run=nullptr;
    Executable(const std::vector<uint8_t>& code,size_t entry) {
        memory=VirtualAlloc(nullptr,code.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!memory)return;
        memcpy(memory,code.data(),code.size());DWORD old=0;
        if(VirtualProtect(memory,code.size(),PAGE_EXECUTE_READ,&old)) {
            FlushInstructionCache(GetCurrentProcess(),memory,code.size());
            run=reinterpret_cast<Function>(static_cast<char*>(memory)+entry);
        }
    }
    ~Executable(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
};
int main(int argc,char** argv) {
    // A byte store consumes only its encoded source byte. A later full
    // overwrite makes the old high bits unobservable, so a byte mask is dead.
    const std::vector<uint8_t> byteStoreMask={0x48,0x89,0xc8,0x48,0x81,0xe0,0xff,0,0,0,
        0x88,0x02,0x4c,0x89,0xc0,0x48,0x39,0xc8,0x9c,0x59,0x48,0x89,0x4a,8,0xc3};
    auto byteStoreProof=tc::simulation_jit::optimize(byteStoreMask.data(),byteStoreMask.size(),0);
    CHECK(byteStoreProof.unusedMasks==1);
    Executable byteStoreA(byteStoreMask,0),byteStoreB(byteStoreProof.code,size_t(byteStoreProof.entry));
    CHECK(byteStoreA.run && byteStoreB.run);
    for(uint64_t input:{0ULL,1ULL,0x123456789abcdef0ULL,0xffffffffffffffffULL}) {
        uint64_t stateA[2]={0x1122334455667788ULL,0},stateB[2]={0x1122334455667788ULL,0};
        CHECK(byteStoreA.run(input,stateA,~input)==~input);
        CHECK(byteStoreB.run(input,stateB,~input)==~input);
        CHECK(stateA[0]==((0x1122334455667788ULL&~0xffULL)|(input&0xff)));
        CHECK(stateA[0]==stateB[0] && (stateA[1]&0x8d5)==(stateB[1]&0x8d5));
    }
    auto highByteStore=byteStoreMask;highByteStore[11]=0x22; // MOV [RDX],AH observes bits 8..15
    auto wideStore=byteStoreMask;wideStore.erase(wideStore.begin()+10,wideStore.begin()+12);
    wideStore.insert(wideStore.begin()+10,{0x48,0x89,0x02});
    for(const auto& code:{highByteStore,wideStore}) {
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(proof.unusedMasks==0);
        Executable a(code,0),b(proof.code.empty()?code:proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(uint64_t input:{0ULL,1ULL,0x123456789abcdef0ULL,0xffffffffffffffffULL}) {
            uint64_t stateA[2]={0x1122334455667788ULL,0},stateB[2]={0x1122334455667788ULL,0};
            CHECK(a.run(input,stateA,~input)==~input && b.run(input,stateB,~input)==~input);
            CHECK(stateA[0]==stateB[0] && (stateA[1]&0x8d5)==(stateB[1]&0x8d5));
            CHECK(stateA[0]==(code==wideStore?(input&0xff):0x1122334455667700ULL));
        }
    }
    const std::vector<uint8_t> extendedByteStore={0x49,0x89,0xc9,0x49,0x81,0xe1,0xff,0,0,0,
        0x44,0x88,0x0a,0x49,0x89,0xc9,0x4c,0x89,0xc0,0x48,0x39,0xc8,0x9c,0x59,0x48,0x89,0x4a,8,0xc3};
    auto extendedByteProof=tc::simulation_jit::optimize(extendedByteStore.data(),extendedByteStore.size(),0);
    CHECK(extendedByteProof.unusedMasks==1);
    Executable extendedByteA(extendedByteStore,0),extendedByteB(extendedByteProof.code,size_t(extendedByteProof.entry));
    CHECK(extendedByteA.run && extendedByteB.run);
    uint64_t byteSeed=0x123456789abcdef0ULL;
    for(unsigned i=0;i<1000;++i) {
        byteSeed^=byteSeed<<13;byteSeed^=byteSeed>>7;byteSeed^=byteSeed<<17;
        uint64_t stateA[2]={0x1122334455667788ULL,0},stateB[2]={0x1122334455667788ULL,0};
        CHECK(extendedByteA.run(byteSeed,stateA,~byteSeed)==~byteSeed);
        CHECK(extendedByteB.run(byteSeed,stateB,~byteSeed)==~byteSeed);
        CHECK(stateA[0]==((0x1122334455667788ULL&~0xffULL)|(byteSeed&0xff)));
        CHECK(stateA[0]==stateB[0] && (stateA[1]&0x8d5)==(stateB[1]&0x8d5));
    }
    // The stored byte can also be the base address: its entire physical
    // register remains live. Clearing pointer bit 8 changes the destination.
    auto addressedByteStore=byteStoreMask;addressedByteStore[11]=0;
    const uint32_t pointerMask=0xfffffeff;memcpy(addressedByteStore.data()+6,&pointerMask,4);
    auto addressedByteProof=tc::simulation_jit::optimize(addressedByteStore.data(),addressedByteStore.size(),0);
    CHECK(addressedByteProof.unusedMasks==0);
    Executable addressedByteA(addressedByteStore,0),addressedByteB(addressedByteProof.code.empty()?addressedByteStore:addressedByteProof.code,size_t(addressedByteProof.entry));
    CHECK(addressedByteA.run && addressedByteB.run);
    auto* bytePage=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(bytePage);
    CHECK((reinterpret_cast<uintptr_t>(bytePage)&0xffff)==0);
    const uint64_t byteAddress=reinterpret_cast<uintptr_t>(bytePage+256);
    uint64_t addressStateA[2]={0,0},addressStateB[2]={0,0};bytePage[0]=bytePage[256]=0xa5;
    CHECK(addressedByteA.run(byteAddress,addressStateA,7)==7 && bytePage[0]==0 && bytePage[256]==0xa5);
    bytePage[0]=bytePage[256]=0xa5;
    CHECK(addressedByteB.run(byteAddress,addressStateB,7)==7 && bytePage[0]==0 && bytePage[256]==0xa5);
    CHECK((addressStateA[1]&0x8d5)==(addressStateB[1]&0x8d5));VirtualFree(bytePage,0,MEM_RELEASE);
    // Even a dead data mask must remain if its flags feed SETcc.
    auto byteStoreFlags=byteStoreMask;byteStoreFlags.insert(byteStoreFlags.begin()+10,{0x41,0x0f,0x95,0xc1,0x44,0x88,0x4a,1});
    byteStoreFlags.insert(byteStoreFlags.begin()+3,{0x48,0x83,0xf9,0});
    auto byteFlagsProof=tc::simulation_jit::optimize(byteStoreFlags.data(),byteStoreFlags.size(),0);CHECK(byteFlagsProof.unusedMasks==0);
    Executable byteFlagsA(byteStoreFlags,0),byteFlagsB(byteFlagsProof.code.empty()?byteStoreFlags:byteFlagsProof.code,size_t(byteFlagsProof.entry));
    CHECK(byteFlagsA.run && byteFlagsB.run);
    for(uint64_t input:{0ULL,1ULL,0x100ULL,0xffULL}) {
        uint64_t stateA[2]={0x1122334455667788ULL,0},stateB[2]={0x1122334455667788ULL,0};
        CHECK(byteFlagsA.run(input,stateA,~input)==~input && byteFlagsB.run(input,stateB,~input)==~input);
        CHECK(((stateA[0]>>8)&0xff)==((input&0xff)!=0));
        CHECK(stateA[0]==stateB[0] && (stateA[1]&0x8d5)==(stateB[1]&0x8d5));
    }
    // Rewriting the same private spill is unnecessary only after an actual
    // store proves its value and write accessibility. Keep the returned value
    // and final flags observable when executing both machine-code versions.
    const std::vector<uint8_t> repeatedStore={0x48,0x83,0xec,40,
        0x48,0x89,0x4c,0x24,16,0x48,0x89,0x4c,0x24,16,
        0x48,0x8b,0x44,0x24,16,0x4c,0x39,0xc0,
        0x9c,0x59,0x48,0x89,0x0a,0x48,0x83,0xc4,40,0xc3};
    auto repeatedProof=tc::simulation_jit::optimize(repeatedStore.data(),repeatedStore.size(),0);
    CHECK(!repeatedProof.code.empty() && repeatedProof.code.size()==repeatedStore.size()-7);
    Executable repeatedA(repeatedStore,0),repeatedB(repeatedProof.code,size_t(repeatedProof.entry));
    CHECK(repeatedA.run && repeatedB.run);
    for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(repeatedA.run(input,&flagsA,~input)==input);
        CHECK(repeatedB.run(input,&flagsB,~input)==input);
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // An indirect alias changes the contents but not write accessibility.
    // Reloading observes its NEW value; only writing that same new value back
    // is removable. Reusing the old stored value would return the wrong result.
    const std::vector<uint8_t> aliasedReadback={0x48,0x83,0xec,40,
        0x48,0x89,0x4c,0x24,16,0x4c,0x8d,0x4c,0x24,16,0x4d,0x89,0x01,
        0x48,0x8b,0x44,0x24,16,0x48,0x89,0x44,0x24,16,0x4c,0x39,0xc0,
        0x9c,0x59,0x48,0x89,0x0a,0x48,0x83,0xc4,40,0xc3};
    auto readbackProof=tc::simulation_jit::optimize(aliasedReadback.data(),aliasedReadback.size(),0);
    CHECK(readbackProof.redundantStores==1);
    Executable readbackA(aliasedReadback,0),readbackB(readbackProof.code,size_t(readbackProof.entry));
    CHECK(readbackA.run && readbackB.run);
    for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(readbackA.run(input,&flagsA,~input)==~input);
        CHECK(readbackB.run(input,&flagsB,~input)==~input);
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    const std::vector<std::vector<uint8_t>> storeBarriers={
        {0x44,0x88,0x44,0x24,17}, // overlapping byte changes the stored value
        {0x4c,0x8d,0x4c,0x24,16,0x4d,0x89,0x01}, // indirect alias writes R8
        {0x48,0x83,0xec,8,0x48,0x83,0xc4,8}, // different stack-pointer epoch
        {0x48,0x83,0xf9,0,0x75,0}, // genuine incoming conditional target
        {0x0f,0x1f,0x00}}; // unmodeled multi-byte NOP is a conservative barrier
    for(const auto& barrier:storeBarriers) {
        std::vector<uint8_t> code(repeatedStore.begin(),repeatedStore.begin()+9);
        code.insert(code.end(),barrier.begin(),barrier.end());
        code.insert(code.end(),repeatedStore.begin()+9,repeatedStore.end());
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(proof.redundantStores==0);
        Executable a(code,0),b(proof.code.empty()?code:proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(input,&flagsA,~input)==input);
            CHECK(b.run(input,&flagsB,~input)==input);
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    auto changedStore=repeatedStore;changedStore.insert(changedStore.begin()+9,{0x4c,0x89,0xc1});
    auto changedProof=tc::simulation_jit::optimize(changedStore.data(),changedStore.size(),0);
    CHECK(changedProof.redundantStores==0);
    Executable changedA(changedStore,0),changedB(changedProof.code.empty()?changedStore:changedProof.code,size_t(changedProof.entry));
    CHECK(changedA.run && changedB.run);uint64_t changedFlagsA=0,changedFlagsB=0;
    CHECK(changedA.run(0x123456789abcdef0ULL,&changedFlagsA,0xfedcba9876543210ULL)==0xfedcba9876543210ULL);
    CHECK(changedB.run(0x123456789abcdef0ULL,&changedFlagsB,0xfedcba9876543210ULL)==0xfedcba9876543210ULL);
    CHECK((changedFlagsA&0x8d5)==(changedFlagsB&0x8d5));
    // Removing the only direct write cannot be justified by a preceding load.
    // The alias initializes the real frame for execution, but proves no exact
    // eight-byte writable slot to this conservative pass.
    auto readOnlyProof=aliasedReadback;readOnlyProof.erase(readOnlyProof.begin()+4,readOnlyProof.begin()+9);
    auto firstStoreProof=tc::simulation_jit::optimize(readOnlyProof.data(),readOnlyProof.size(),0);
    CHECK(firstStoreProof.redundantStores==0);
    Executable firstA(readOnlyProof,0),firstB(firstStoreProof.code.empty()?readOnlyProof:firstStoreProof.code,size_t(firstStoreProof.entry));
    CHECK(firstA.run && firstB.run);uint64_t firstFlagsA=0,firstFlagsB=0;
    CHECK(firstA.run(0,&firstFlagsA,0x8765432112345678ULL)==0x8765432112345678ULL);
    CHECK(firstB.run(0,&firstFlagsB,0x8765432112345678ULL)==0x8765432112345678ULL);
    CHECK((firstFlagsA&0x8d5)==(firstFlagsB&0x8d5));
    // Zero-extension from a 32-bit load changes the high half of the next
    // 64-bit store, despite matching address and source register names.
    auto narrowedReadback=repeatedStore;
    narrowedReadback.erase(narrowedReadback.begin()+9,narrowedReadback.begin()+19);
    narrowedReadback.insert(narrowedReadback.begin()+9,{0x8b,0x44,0x24,16,0x48,0x89,0x44,0x24,16});
    auto narrowedProof=tc::simulation_jit::optimize(narrowedReadback.data(),narrowedReadback.size(),0);
    CHECK(narrowedProof.redundantStores==0);
    Executable narrowedA(narrowedReadback,0),narrowedB(narrowedProof.code.empty()?narrowedReadback:narrowedProof.code,size_t(narrowedProof.entry));
    CHECK(narrowedA.run && narrowedB.run);uint64_t narrowedFlagsA=0,narrowedFlagsB=0;
    CHECK(narrowedA.run(0x12345678000000ffULL,&narrowedFlagsA,0)==0xff);
    CHECK(narrowedB.run(0x12345678000000ffULL,&narrowedFlagsB,0)==0xff);
    CHECK((narrowedFlagsA&0x8d5)==(narrowedFlagsB&0x8d5));
    std::vector<uint8_t> callStore(repeatedStore.begin(),repeatedStore.begin()+9);
    callStore.insert(callStore.end(),{0xe8,0,0,0,0});
    callStore.insert(callStore.end(),repeatedStore.begin()+9,repeatedStore.end());
    const int32_t storeCallDelta=int32_t(callStore.size()-14);memcpy(callStore.data()+10,&storeCallDelta,4);
    callStore.push_back(0xc3);
    auto storeCallProof=tc::simulation_jit::optimize(callStore.data(),callStore.size(),0);
    CHECK(storeCallProof.redundantStores==0);
    Executable storeCallA(callStore,0),storeCallB(storeCallProof.code.empty()?callStore:storeCallProof.code,size_t(storeCallProof.entry));
    CHECK(storeCallA.run && storeCallB.run);uint64_t storeCallFlagsA=0,storeCallFlagsB=0;
    CHECK(storeCallA.run(0x1122334455667788ULL,&storeCallFlagsA,0)==0x1122334455667788ULL);
    CHECK(storeCallB.run(0x1122334455667788ULL,&storeCallFlagsB,0)==0x1122334455667788ULL);
    CHECK((storeCallFlagsA&0x8d5)==(storeCallFlagsB&0x8d5));
    // Sign extension changes only the upper bits. Both sides of a real
    // conditional branch overwrite them before any observer can read them.
    const std::vector<uint8_t> unusedExtension={0x48,0x89,0xc8,0x48,0x0f,0xbe,0xc0,
        0x3c,0,0x75,5,0x4c,0x89,0xc0,0xeb,3,0x48,0x89,0xc8,
        0x9c,0x59,0x48,0x89,0x0a,0xc3};
    auto unusedProof=tc::simulation_jit::optimize(unusedExtension.data(),unusedExtension.size(),0);
    CHECK(unusedProof.removed==1 && unusedProof.code.size()==unusedExtension.size()-4);
    Executable unusedA(unusedExtension,0),unusedB(unusedProof.code,size_t(unusedProof.entry));CHECK(unusedA.run && unusedB.run);
    for(uint64_t input:{0ULL,1ULL,0x80ULL,0xffULL,0x8000000000000000ULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(unusedA.run(input,&flagsA,~input)==unusedB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    auto oversizedGraph=unusedExtension;oversizedGraph.insert(oversizedGraph.end()-1,32768,0x90);
    CHECK(tc::simulation_jit::optimize(oversizedGraph.data(),oversizedGraph.size(),0).unusedExtensions==0);
    // This unreachable backwards chain takes more than 64 sweeps to converge.
    // Never use the partially solved graph to delete the earlier extension.
    auto slowGraph=unusedExtension;size_t previousTarget=slowGraph.size()-1;
    for(unsigned i=0;i<70;++i) {
        const size_t at=slowGraph.size();const int32_t delta=int32_t(previousTarget)-int32_t(at+5);
        slowGraph.insert(slowGraph.end(),{0xe9,0,0,0,0});memcpy(slowGraph.data()+at+1,&delta,4);previousTarget=at;
    }
    CHECK(tc::simulation_jit::optimize(slowGraph.data(),slowGraph.size(),0).unusedExtensions==0);
    // One live path, a full-width compare, AH and an unknown call must each
    // retain the high bits. Byte operands used as addresses still need 64 bits.
    auto oneLivePath=unusedExtension;oneLivePath[10]=8;
    auto fullCompare=unusedExtension;fullCompare.erase(fullCompare.begin()+7,fullCompare.begin()+9);
    fullCompare.insert(fullCompare.begin()+7,{0x48,0x83,0xf8,0});
    auto highCompare=unusedExtension;highCompare.erase(highCompare.begin()+7,highCompare.begin()+9);
    highCompare.insert(highCompare.begin()+7,{0x80,0xfc,0});
    auto called=unusedExtension;called.insert(called.begin()+7,{0xe8,0,0,0,0});called.push_back(0xc3);
    const int32_t callDelta=int32_t(called.size()-1-12);memcpy(called.data()+8,&callDelta,4);
    for(const auto& code:{oneLivePath,fullCompare,highCompare,called}) {
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(proof.unusedExtensions==0);
        Executable a(code,0),b(proof.code.empty()?code:proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(uint64_t input:{0ULL,1ULL,0x80ULL,0xffULL,0x100ULL,0x8000000000000000ULL}) {
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(input,&flagsA,~input)==b.run(input,&flagsB,~input));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    auto addressed=unusedExtension;addressed[7]=0x3a;addressed[8]=0x00; // CMP AL,[RAX]
    CHECK(tc::simulation_jit::optimize(addressed.data(),addressed.size(),0).unusedExtensions==0);
    // Demand propagates over the backward edge to a fixed point. A later
    // full overwrite makes extension dead; a returned signed result keeps it.
    uint64_t flowSeed=0xfedcba9876543210ULL;
    for(bool overwrite:{false,true}) {
        std::vector<uint8_t> code={0x4c,0x89,0xc0,0x48,0x0f,0xbe,0xc0,0x3c,0};
        if(overwrite)code.insert(code.end(),{0x4c,0x89,0xc0});
        code.insert(code.end(),{0x48,0x83,0xe9,1});
        const int backward=int(3)-int(code.size()+2);code.insert(code.end(),{0x75,uint8_t(int8_t(backward))});
        code.insert(code.end(),{0x9c,0x59,0x48,0x89,0x0a,0xc3});
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(proof.unusedExtensions==(overwrite?1u:0u));
        Executable a(code,0),b(proof.code.empty()?code:proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            flowSeed^=flowSeed<<13;flowSeed^=flowSeed>>7;flowSeed^=flowSeed<<17;
            uint64_t flagsA=0,flagsB=0;const uint64_t iterations=1+(i%31);
            CHECK(a.run(iterations,&flagsA,flowSeed)==b.run(iterations,&flagsB,flowSeed));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    // A full copy is dead across a branch if both successors overwrite it.
    // The branch condition reads RCX; the old RAX value is never consumed.
    const std::vector<uint8_t> branchDeadCopy={0x48,0x89,0xc8,0x48,0x83,0xf9,0,
        0x75,5,0x4c,0x89,0xc0,0xeb,3,0x48,0x89,0xc8,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    auto flowProof=tc::simulation_jit::optimize(branchDeadCopy.data(),branchDeadCopy.size(),0);
    CHECK(flowProof.removed==1 && flowProof.code.size()==branchDeadCopy.size()-3);
    Executable flowA(branchDeadCopy,0),flowB(flowProof.code,size_t(flowProof.entry));CHECK(flowA.run && flowB.run);
    for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(flowA.run(input,&flagsA,~input)==flowB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // The next mask consumes just bit zero, even through a full-width copy.
    // The first byte mask has no data use and its flags are overwritten.
    const std::vector<uint8_t> unusedMask={0x48,0x89,0xc8,0x48,0x81,0xe0,0xff,0,0,0,
        0x49,0x89,0xc0,0x49,0x83,0xe0,1,0x4c,0x89,0xc0,0x49,0x83,0xf8,0,0x9c,0x59,
        0x48,0x89,0x0a,0xc3};
    unusedProof=tc::simulation_jit::optimize(unusedMask.data(),unusedMask.size(),0);
    CHECK(unusedProof.removed==1 && unusedProof.code.size()==unusedMask.size()-7);
    Executable unusedMaskA(unusedMask,0),unusedMaskB(unusedProof.code,size_t(unusedProof.entry));CHECK(unusedMaskA.run && unusedMaskB.run);
    auto observedMask=unusedMask;observedMask.insert(observedMask.begin()+10,{0x41,0x0f,0x95,0xc1});
    CHECK(tc::simulation_jit::optimize(observedMask.data(),observedMask.size(),0).unusedMasks==0);
    for(uint64_t input:{0ULL,1ULL,0x80ULL,0xffULL,0x8000000000000000ULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(unusedMaskA.run(input,&flagsA,~input)==unusedMaskB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // A private spill is reloaded after its original source changes. The
    // unchanged full-width alias must supply the old value, with flags intact.
    const std::vector<uint8_t> spillAlias={0x48,0x83,0xec,40,0x48,0x89,0x4c,0x24,16,
        0x48,0x89,0xc8,0x4c,0x89,0xc1,0x4c,0x8b,0x4c,0x24,16,0x4c,0x31,0xc8,
        0x9c,0x59,0x48,0x89,0x0a,0x48,0x83,0xc4,40,0xc3};
    auto spillResult=tc::simulation_jit::optimize(spillAlias.data(),spillAlias.size(),0);
    CHECK(!spillResult.code.empty() && spillResult.code.size()<spillAlias.size());
    Executable spillA(spillAlias,0),spillB(spillResult.code,size_t(spillResult.entry));CHECK(spillA.run && spillB.run);
    for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(spillA.run(input,&flagsA,~input)==spillB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // The runtime must accept a rewritten sequence even when no instruction
    // is deleted: replacing a load with MOV is itself an optimization.
    const std::vector<uint8_t> forwardOnly={0x48,0x83,0xec,40,0x48,0x89,0x4c,0x24,16,
        0x4c,0x8b,0x4c,0x24,16,0x4c,0x89,0xc8,0x48,0x83,0xc4,40,0xc3};
    auto forwardProof=tc::simulation_jit::optimize(forwardOnly.data(),forwardOnly.size(),0);
    CHECK(forwardProof.removed==0 && forwardProof.specialized==0 && forwardProof.stackForwards==1 && !forwardProof.code.empty());
    Executable forwardA(forwardOnly,0),forwardB(forwardProof.code,size_t(forwardProof.entry));CHECK(forwardA.run && forwardB.run);
    uint64_t forwardScratch=0;
    CHECK(forwardA.run(0x8765432112345678ULL,&forwardScratch,0)==forwardB.run(0x8765432112345678ULL,&forwardScratch,0));
    // Two zero-extended byte loads separated by a disjoint spill share the
    // same value. An overlapping byte write must invalidate that value.
    const std::vector<uint8_t> byteReload={0x48,0x83,0xec,40,0x88,0x4c,0x24,16,
        0x48,0x0f,0xb6,0x44,0x24,16,0x44,0x88,0x44,0x24,17,
        0x4c,0x0f,0xb6,0x4c,0x24,16,0x4c,0x31,0xc8,0x9c,0x59,0x48,0x89,0x0a,
        0x48,0x83,0xc4,40,0xc3};
    auto byteResult=tc::simulation_jit::optimize(byteReload.data(),byteReload.size(),0);
    CHECK(byteResult.stackForwards==1);
    Executable byteA(byteReload,0),byteB(byteResult.code,size_t(byteResult.entry));CHECK(byteA.run && byteB.run);
    for(uint64_t input:{0ULL,1ULL,0x80ULL,0xffULL,0x123456789abcdef0ULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(byteA.run(input,&flagsA,~input)==byteB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // MOVZX plus a right shift already guarantees a byte-sized value. The
    // following byte mask can disappear only if its flags are overwritten.
    const std::vector<uint8_t> boundedShift={0x48,0x0f,0xb6,0xc1,0x48,0xc1,0xe8,6,
        0x48,0x81,0xe0,0xff,0,0,0,0x4c,0x39,0xc0,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    auto boundedResult=tc::simulation_jit::optimize(boundedShift.data(),boundedShift.size(),0);
    CHECK(boundedResult.redundantMasks==1);
    Executable boundedA(boundedShift,0),boundedB(boundedResult.code,size_t(boundedResult.entry));CHECK(boundedA.run && boundedB.run);
    for(uint64_t input:{0ULL,1ULL,0x80ULL,0xffULL,0x123456789abcdef0ULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(boundedA.run(input,&flagsA,~input)==boundedB.run(input,&flagsB,~input));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // Overlap, indirect aliases, stack-pointer changes and entry joins are
    // barriers. All variants execute on a real allocated private stack frame.
    const std::vector<uint8_t> spillPrefix={0x48,0x83,0xec,40,0x48,0x89,0x4c,0x24,16,0x48,0x8b,0x44,0x24,16};
    const std::vector<uint8_t> spillSuffix={0x4c,0x8b,0x4c,0x24,16,0x4c,0x31,0xc8,
        0x9c,0x59,0x48,0x89,0x0a,0x48,0x83,0xc4,40,0xc3};
    for(const auto& barrier:{
        std::vector<uint8_t>{0x44,0x88,0x44,0x24,17}, // overlapping byte store
        std::vector<uint8_t>{0x4c,0x8d,0x5c,0x24,16,0x4d,0x89,0x03}, // indirect alias store
        std::vector<uint8_t>{0x48,0x83,0xec,8,0x48,0x83,0xc4,8},
        std::vector<uint8_t>{0x75,0}, // genuine incoming target at the reload
        std::vector<uint8_t>{0x4c,0x89,0xc1,0xb8,7,0,0,0}}) { // no surviving value alias
        auto code=spillPrefix;code.insert(code.end(),barrier.begin(),barrier.end());
        code.insert(code.end(),spillSuffix.begin(),spillSuffix.end());
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(proof.stackForwards==1);
        Executable a(code,0),b(proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(uint64_t input:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(input,&flagsA,~input)==b.run(input,&flagsB,~input));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    auto overlappingByte=byteReload;overlappingByte[18]=16;
    CHECK(tc::simulation_jit::optimize(overlappingByte.data(),overlappingByte.size(),0).stackForwards==0);
    auto widthMismatch=spillPrefix;widthMismatch.insert(widthMismatch.end(),{0x44,0x8b,0x4c,0x24,16});
    widthMismatch.insert(widthMismatch.end(),spillSuffix.begin()+5,spillSuffix.end());
    auto widthProof=tc::simulation_jit::optimize(widthMismatch.data(),widthMismatch.size(),0);CHECK(widthProof.stackForwards==1);
    Executable widthA(widthMismatch,0),widthB(widthProof.code,size_t(widthProof.entry));CHECK(widthA.run && widthB.run);
    uint64_t widthFlagsA=0,widthFlagsB=0;
    CHECK(widthA.run(0x123456789abcdef0ULL,&widthFlagsA,0)==widthB.run(0x123456789abcdef0ULL,&widthFlagsB,0));
    CHECK((widthFlagsA&0x8d5)==(widthFlagsB&0x8d5));
    // Different sources, signed shifts, 32-bit zero extension and effective
    // shift counts are exercised with random live machine-code comparisons.
    uint64_t boundSeed=0xabcdef0123456789ULL;
    for(bool wide:{false,true})for(uint8_t shiftOp:{uint8_t(0xe0),uint8_t(0xe8),uint8_t(0xf8)})
    for(uint8_t count:{uint8_t(1),uint8_t(7),uint8_t(31),uint8_t(32),uint8_t(63),uint8_t(64),uint8_t(255)}) {
        std::vector<uint8_t> code={0x48,0x89,0xc8};
        if(wide)code.push_back(0x48);
        code.insert(code.end(),{0xc1,shiftOp,count,0x48,0x89,0xc1,0x48,0x81,0xe1,0xff,0,0,0,
            0x4c,0x39,0xc1,0x9c,0x58,0x48,0x89,0x02,0x48,0x89,0xc8,0xc3});
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);
        Executable a(code,0),b(proof.code.empty()?code:proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            boundSeed^=boundSeed<<13;boundSeed^=boundSeed>>7;boundSeed^=boundSeed<<17;
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(boundSeed,&flagsA,~boundSeed)==b.run(boundSeed,&flagsB,~boundSeed));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    for(const auto& observer:{std::vector<uint8_t>{0x9c,0x41,0x59},
                              std::vector<uint8_t>{0x41,0x0f,0x95,0xc1},
                              std::vector<uint8_t>{0x49,0x83,0xd0,0}}) {
        auto code=boundedShift;code.insert(code.begin()+15,observer.begin(),observer.end());
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).redundantMasks==0);
    }
    // Narrow signed normalization preserves the entire result, not just its
    // low byte. It should use one MOVSX/MOVSXD when flags are overwritten.
    uint64_t signedSeed=0x76543210fedcba98ULL;
    for(uint8_t reg:{uint8_t(0),uint8_t(5),uint8_t(8),uint8_t(9),uint8_t(13)})
    for(uint32_t count:{32u,48u,56u,96u,112u,120u}) {
        const uint8_t rex=reg>=8?0x49:0x48,low=reg&7;
        std::vector<uint8_t> code={rex,0x89,uint8_t(0xc8|low),0xb9,0,0,0,0,
            rex,0xd3,uint8_t(0xe0|low),reg>=8?uint8_t(0x4c):uint8_t(0x48),0x89,uint8_t(0xc1|(low<<3)),
            0xb9,0,0,0,0,rex,0xd3,uint8_t(0xf8|low),rex,0x39,uint8_t(0xc8|low),
            0x9c,0x59,0x48,0x89,0x0a};
        memcpy(code.data()+4,&count,4);memcpy(code.data()+15,&count,4);
        if(reg!=0)code.insert(code.end(),{reg>=8?uint8_t(0x4c):uint8_t(0x48),0x89,uint8_t(0xc0|(low<<3))});
        if(reg==5 || reg==13) {
            if(reg>=8)code.insert(code.begin(),0x41);
            code.insert(code.begin()+(reg>=8?1:0),uint8_t(0x50|low));
            if(reg>=8)code.push_back(0x41);
            code.push_back(uint8_t(0x58|low));
        }
        code.push_back(0xc3);
        auto proof=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(proof.specialized==0 && proof.removed>=3);
        Executable a(code,0),b(proof.code,size_t(proof.entry));CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            signedSeed^=signedSeed<<13;signedSeed^=signedSeed>>7;signedSeed^=signedSeed<<17;
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(signedSeed,&flagsA,0)==b.run(signedSeed,&flagsB,0));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    const std::vector<uint8_t> signedBlock={0xb9,56,0,0,0,0x48,0xd3,0xe0,
        0x48,0x89,0xc1,0xb9,56,0,0,0,0x48,0xd3,0xf8};
    for(const auto& observer:{std::vector<uint8_t>{0x9c,0x41,0x59},
                              std::vector<uint8_t>{0x41,0x0f,0x95,0xc1},
                              std::vector<uint8_t>{0x49,0x83,0xd0,0}}) {
        auto code=std::vector<uint8_t>{0x48,0x89,0xc8};code.insert(code.end(),signedBlock.begin(),signedBlock.end());
        code.insert(code.end(),observer.begin(),observer.end());code.insert(code.end(),{0x48,0x39,0xc8,0xc3});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).signedExtensions==0);
    }
    for(size_t entry:{size_t(5),size_t(8),size_t(11),size_t(16)}) {
        auto code=signedBlock;code.insert(code.end(),{0x48,0x39,0xc8,0xc3});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),int64_t(entry)).signedExtensions==0);
    }
    for(uint8_t count:{uint8_t(31),uint8_t(49),uint8_t(57),uint8_t(63)}) {
        auto code=signedBlock;code[1]=count;code[12]=count;code.insert(code.end(),{0x48,0x39,0xc8,0xc3});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).signedExtensions==0);
    }
    auto mismatchedSigned=signedBlock;mismatchedSigned[12]=48;
    mismatchedSigned.insert(mismatchedSigned.end(),{0x48,0x39,0xc8,0xc3});
    CHECK(tc::simulation_jit::optimize(mismatchedSigned.data(),mismatchedSigned.size(),0).signedExtensions==0);
    // rax=rcx; rcx=rax (redundant inverse copy); rax+=r8; capture flags.
    const std::vector<uint8_t> inverse={0x48,0x89,0xc8,0x48,0x89,0xc1,0x4c,0x01,0xc0,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    auto result=tc::simulation_jit::optimize(inverse.data(),inverse.size(),0);
    CHECK(result.removed==1 && result.code.size()==inverse.size()-3 && result.entry==0);
    Executable original(inverse,0),optimized(result.code,size_t(result.entry));
    CHECK(original.run && optimized.run);
    uint64_t seed=0x123456789abcdef0ULL;
    // A direct jump to the immediately following instruction is a no-op.
    // Values and all defined flags survive, including entry at the jump itself.
    for(bool shortJump:{false,true}) {
        std::vector<uint8_t> code={0x48,0x89,0xc8};
        if(shortJump)code.insert(code.end(),{0xeb,0});
        else code.insert(code.end(),{0xe9,0,0,0,0});
        code.insert(code.end(),{0x4c,0x01,0xc0,0x9c,0x59,0x48,0x89,0x0a,0xc3});
        auto jumpResult=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(jumpResult.removed==1);
        Executable a(code,0),b(jumpResult.code,size_t(jumpResult.entry));CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(seed,&flagsA,~seed)==b.run(seed,&flagsB,~seed));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
        auto entryCode=code;entryCode.insert(entryCode.begin()+3+(shortJump?2:5),{0x48,0x89,0xc8});
        jumpResult=tc::simulation_jit::optimize(entryCode.data(),entryCode.size(),3);CHECK(jumpResult.removed>=1);
        Executable entryA(entryCode,3),entryB(jumpResult.code,size_t(jumpResult.entry));CHECK(entryA.run && entryB.run);
        uint64_t flagsA=0,flagsB=0;
        CHECK(entryA.run(seed,&flagsA,0)==entryB.run(seed,&flagsB,0));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // A different branch may target the removed jump: relocate it to the
    // following instruction, while retaining that genuine merge boundary.
    const std::vector<uint8_t> jumpIntoFallthrough={0xeb,3,0x48,0x89,0xc8,0xe9,0,0,0,0,0x4c,0x89,0xc0,0xc3};
    auto incomingResult=tc::simulation_jit::optimize(jumpIntoFallthrough.data(),jumpIntoFallthrough.size(),0);
    CHECK(incomingResult.fallthroughJumps==1 && incomingResult.deadCopies==0);
    Executable incomingA(jumpIntoFallthrough,0),incomingB(incomingResult.code,size_t(incomingResult.entry));CHECK(incomingA.run && incomingB.run);
    uint64_t incomingFlags=0;
    CHECK(incomingA.run(7,&incomingFlags,31)==31 && incomingB.run(7,&incomingFlags,31)==31);
    // LOOP changes RCX even for a zero displacement, and CALL-next exposes PC.
    const std::vector<uint8_t> loopToNext={0xe2,0,0x48,0x89,0xc8,0xc3};
    CHECK(tc::simulation_jit::optimize(loopToNext.data(),loopToNext.size(),0).fallthroughJumps==0);
    for(unsigned i=0;i<10000;++i) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        uint64_t flagsA=0,flagsB=0;
        const auto a=original.run(seed,&flagsA,~seed+i);
        const auto b=optimized.run(seed,&flagsB,~seed+i);
        CHECK(a==b && (flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // Carry/zero/sign/parity flags survive a shift with CL=0.
    const std::vector<uint8_t> shift={0x48,0x89,0xc8,0x48,0x39,0xc0,0xb9,0,0,0,0,0x48,0xd3,0xe0,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    result=tc::simulation_jit::optimize(shift.data(),shift.size(),0);
    CHECK(result.removed==1);
    Executable shiftA(shift,0),shiftB(result.code,size_t(result.entry));
    for(uint64_t value:{0ULL,1ULL,0x8000000000000000ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(shiftA.run(value,&flagsA,0)==shiftB.run(value,&flagsB,0));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // A branch into the second copy invalidates the inverse-copy proof.
    const std::vector<uint8_t> branch={0x48,0x89,0xc8,0x4c,0x89,0xc1,0xeb,3,0x48,0x89,0xc8,0x48,0x89,0xc1,0x48,0x89,0xc8,0xc3};
    result=tc::simulation_jit::optimize(branch.data(),branch.size(),0);
    CHECK(result.removed==2 && result.flowCopies==1); // inverse copy plus RCX overwritten across JMP
    Executable branchA(branch,0),branchB(result.code,size_t(result.entry));
    uint64_t flags=0;
    CHECK(branchA.run(0xffff000000000123ULL,&flags,99)==branchB.run(0xffff000000000123ULL,&flags,99));
    // A helper before the entry shrinks; entry and short jump must relocate.
    const std::vector<uint8_t> relocation={0x4d,0x89,0xc0,0xc3,0x48,0x89,0xc8,0xeb,3,0x4d,0x89,0xc0,0xc3};
    result=tc::simulation_jit::optimize(relocation.data(),relocation.size(),4);
    CHECK(result.removed==2 && result.entry==1);
    Executable relocationB(result.code,size_t(result.entry));
    CHECK(relocationB.run(0xdeadbeef00000001ULL,&flags,0)==0xdeadbeef00000001ULL);
    const std::vector<uint8_t> narrow={0x89,0xc8,0x89,0xc1,0x48,0x89,0xc8,0xc3};
    CHECK(tc::simulation_jit::optimize(narrow.data(),narrow.size(),0).removed==0);
    for(const std::vector<uint8_t>& unknown:{
        std::vector<uint8_t>{0x48,0x8b,0x05,0,0,0,0,0xc3}, // RIP-relative data
        std::vector<uint8_t>{0x48,0x89,0xc0,0x67,0x48,0x8b,0x05,0,0,0,0,0xc3}, // EIP-relative with address override
        std::vector<uint8_t>{0x48,0x89,0xc0,0x67,0x48,0x8b,0x06,0xc3}, // HDE misdecodes address override with rm=6
        std::vector<uint8_t>{0x48,0x89,0xc0,0x67,0x48,0x8b,0x80,0,0,0,0,0xc3}, // HDE misdecodes mod=2 displacement
        std::vector<uint8_t>{0xff,0xe0}, // indirect branch
        std::vector<uint8_t>{0x48,0x89,0xc0,0xe8,0,0,0,0,0x58,0xc3}, // call/pop observes code position
        std::vector<uint8_t>{0xeb,1,0x48,0x89,0xc0,0xc3}, // target inside instruction
        std::vector<uint8_t>{0x48}}) // incomplete encoding
        CHECK(tc::simulation_jit::optimize(unknown.data(),unknown.size(),0).removed==0);
    // Constant-count forms preserve values. Only specialize shifts whose
    // flags are overwritten before any observer, including SETcc/calls/RET.
    for(uint8_t operation:{uint8_t(0xe0),uint8_t(0xe8),uint8_t(0xf8)})
    for(uint32_t count:{1u,7u,32u,56u,63u,64u,255u,256u}) {
        std::vector<uint8_t> code={0x48,0x89,0xc8,0xb9,0,0,0,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0,0x9c,0x59,0x48,0x89,0x0a,0xc3};
        memcpy(code.data()+4,&count,4);
        code[10]=operation;
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.specialized==1 && result.removed==0 && result.code.size()==code.size()+1);
        Executable a(code,0),b(result.code,size_t(result.entry));
        CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(seed,&flagsA,~seed)==b.run(seed,&flagsB,~seed));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    for(const auto& code:{
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0x9c,0x59,0x4c,0x39,0xc0,0xc3}, // PUSHF observes flags
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0x0f,0x90,0xc1,0x4c,0x39,0xc0,0xc3}, // SETO observes OF
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0x4c,0x11,0xc0,0x4c,0x39,0xc0,0xc3}, // ADC consumes carry
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0xe8,4,0,0,0,0x4c,0x39,0xc0,0xc3,0xc3}, // CALL can inspect flags
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0xc3}, // flags live at return
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe4,0x4c,0x39,0xc0,0xc3}, // never rewrite RSP
        std::vector<uint8_t>{0x4c,0x89,0xc0,0xb9,7,0,0,0,0xeb,5,0xb9,56,0,0,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0,0xc3}}) // target bypasses constant MOV
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).specialized==0);
    // The MOV remains observable after the shift, including upper count bits
    // that hardware masks out of the effective shift count.
    for(uint32_t count:{7u,64u,255u,256u,0xffffffffu}) {
        std::vector<uint8_t> code={0x48,0x89,0xc8,0xb9,0,0,0,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0,0x48,0x89,0xc8,0xc3};
        memcpy(code.data()+4,&count,4);
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.specialized==1);
        Executable a(code,0),b(result.code,size_t(result.entry));
        CHECK(a.run && b.run);
        CHECK(a.run(0xfedcba9876543210ULL,&flags,0)==count);
        CHECK(b.run(0xfedcba9876543210ULL,&flags,0)==count);
    }
    // HDE records REX via flags/rex_b, not its rex byte. A 16-bit R9W
    // immediate can also be five bytes long; it never defines ECX/CL.
    for(uint8_t immediate:{uint8_t(0),uint8_t(1)}) {
        const std::vector<uint8_t> prefixed={0x66,0x41,0xb9,immediate,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0,0xc3};
        result=tc::simulation_jit::optimize(prefixed.data(),prefixed.size(),0);
        CHECK(result.removed==0 && result.specialized==0);
    }
    const std::vector<uint8_t> expanded={0xeb,15,0x48,0x89,0xc8,0xb9,56,0,0,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0,0xc3,0x48,0x89,0xc8,0xc3};
    for(int64_t entry:{0,17}) {
        result=tc::simulation_jit::optimize(expanded.data(),expanded.size(),entry);
        CHECK(result.specialized==1 && result.code[1]==16);
        Executable a(expanded,size_t(entry)),b(result.code,size_t(result.entry));
        CHECK(a.run(0xfedcba9876543210ULL,&flags,0)==b.run(0xfedcba9876543210ULL,&flags,0));
    }
    // Growing a shift may exhaust a short jump's signed-byte range. Keep the
    // original whole block at both limits, and relocate adjacent valid cases.
    const std::vector<uint8_t> shiftBlock={0xb9,7,0,0,0,0x48,0xd3,0xe0,0x4c,0x39,0xc0};
    for(unsigned distance:{126u,127u}) {
        std::vector<uint8_t> code={0xeb,uint8_t(distance)};
        code.insert(code.end(),shiftBlock.begin(),shiftBlock.end());
        code.resize(2+distance,0x90);code.push_back(0xc3);
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(distance==127 ? result.code.empty() : result.specialized==1 && result.code[1]==127);
    }
    for(unsigned distance:{127u,128u}) {
        auto code=shiftBlock;code.resize(distance-2,0x90);
        code.push_back(0xeb);code.push_back(uint8_t(-int(distance)));code.push_back(0xc3);
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(distance==128 ? result.code.empty() : result.specialized==1 && result.code[distance]==128);
    }
    // Narrow-width normalization: shift left/right by the same count. RCX
    // contains the final full immediate; flags are observed only after CMP.
    for(uint8_t reg:{uint8_t(0),uint8_t(8),uint8_t(9),uint8_t(12),uint8_t(15)})
    for(uint32_t count:{32u,56u,57u,63u,255u}) {
        const uint8_t rex=reg>=8?0x49:0x48,low=reg&7;
        std::vector<uint8_t> code={rex,0x89,uint8_t(0xc8|low),0xb9,0,0,0,0,rex,0xd3,uint8_t(0xe0|low),reg>=8?uint8_t(0x4c):uint8_t(0x48),0x89,uint8_t(0xc1|(low<<3)),0xb9,0,0,0,0,rex,0xd3,uint8_t(0xe8|low),rex,0x39,uint8_t(0xc8|low),0x9c,0x59,0x48,0x89,0x0a};
        if(reg!=0)code.insert(code.end(),{0x4c,0x89,uint8_t(0xc0|(low<<3))});
        code.push_back(0xc3);
        memcpy(code.data()+4,&count,4);memcpy(code.data()+15,&count,4);
        if(reg>=12) {
            code.insert(code.begin(),{0x41,uint8_t(0x50|low)}); // preserve Win64 nonvolatile register
            code.insert(code.end()-1,{0x41,uint8_t(0x58|low)});
        }
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.removed==3 && result.code.size()<code.size());
        Executable a(code,0),b(result.code,size_t(result.entry));
        CHECK(a.run && b.run);
        for(unsigned i=0;i<1000;++i) {
            seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(seed,&flagsA,0)==b.run(seed,&flagsB,0));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    const std::vector<uint8_t> maskBlock={0xb9,56,0,0,0,0x48,0xd3,0xe0,0x48,0x89,0xc1,0xb9,56,0,0,0,0x48,0xd3,0xe8};
    // Memory CMP overwrites arithmetic flags just as register CMP does.
    auto memoryFlagsMask=maskBlock;
    memoryFlagsMask.insert(memoryFlagsMask.begin(),{0x48,0x89,0xc8});
    memoryFlagsMask.insert(memoryFlagsMask.end(),{0x80,0x3a,0,0x9c,0x59,0x48,0x89,0x0a,0xc3});
    result=tc::simulation_jit::optimize(memoryFlagsMask.data(),memoryFlagsMask.size(),0);
    CHECK(result.masked==1);
    Executable memoryFlagsA(memoryFlagsMask,0),memoryFlagsB(result.code,size_t(result.entry));CHECK(memoryFlagsA.run && memoryFlagsB.run);
    for(uint64_t input:{0ULL,1ULL,255ULL,256ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0x81,flagsB=0x81;
        CHECK(memoryFlagsA.run(input,&flagsA,0)==memoryFlagsB.run(input,&flagsB,0));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // Some generated blocks have many stores before the next arithmetic
    // overwrite; these preserve flags, but an intervening SETcc still blocks.
    for(bool observesFlags:{false,true}) {
        auto code=std::vector<uint8_t>{0x48,0x89,0xc8};
        code.insert(code.end(),maskBlock.begin(),maskBlock.end());
        for(unsigned store=0;store<100;++store)code.insert(code.end(),{0x88,0x02});
        if(observesFlags)code.insert(code.end(),{0x0f,0x92,0xc1}); // SETB reads CF
        code.insert(code.end(),{0x48,0x39,0xc8,0x4d,0x89,0xc9,0x9c,0x59,0x48,0x89,0x0a,0xc3});
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.masked==(observesFlags?0u:1u));
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        for(uint64_t input:{0ULL,1ULL,255ULL,256ULL,0xffffffffffffffffULL}) {
            uint64_t flagsA=0,flagsB=0;
            CHECK(a.run(input,&flagsA,0)==b.run(input,&flagsB,0));
            CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
        }
    }
    auto originalCompilerMask=maskBlock;
    originalCompilerMask.insert(originalCompilerMask.begin()+11,{0x48,0x89,0xc8}); // inverse copy removed by the earlier pass
    originalCompilerMask.insert(originalCompilerMask.end(),{0x48,0x39,0xc8,0xc3});
    result=tc::simulation_jit::optimize(originalCompilerMask.data(),originalCompilerMask.size(),0);
    CHECK(result.masked==1 && result.removed==4);
    // A targeted self-copy disappears in the first pass, but still blocks
    // fusion because its branch/entry would bypass the initial constant.
    auto targetedNoop=maskBlock;
    targetedNoop.insert(targetedNoop.begin()+5,{0x4d,0x89,0xc9});
    targetedNoop.insert(targetedNoop.end(),{0x48,0x39,0xc8,0xc3});
    CHECK(tc::simulation_jit::optimize(targetedNoop.data(),targetedNoop.size(),5).masked==0);
    targetedNoop.insert(targetedNoop.begin(),{0xeb,5});
    CHECK(tc::simulation_jit::optimize(targetedNoop.data(),targetedNoop.size(),0).masked==0);
    // Retain the complete final ECX value, not only the six effective bits.
    for(uint32_t firstCount:{32u,56u})
    for(uint32_t finalCount:{firstCount,firstCount+64,0xabcdef00u|firstCount}) {
        auto code=std::vector<uint8_t>{0x48,0x89,0xc8};
        code.insert(code.end(),maskBlock.begin(),maskBlock.end());
        memcpy(code.data()+4,&firstCount,4);
        memcpy(code.data()+15,&finalCount,4);
        code.insert(code.end(),{0x48,0x39,0xc8,0x48,0x89,0xc8,0xc3});
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.masked==1);
        Executable a(code,0),b(result.code,size_t(result.entry));
        CHECK(a.run && b.run);
        CHECK(a.run(0xffffffffffffffffULL,&flags,0)==finalCount);
        CHECK(b.run(0xffffffffffffffffULL,&flags,0)==finalCount);
    }
    for(unsigned variant=0;variant<8;++variant) {
        auto code=maskBlock;
        switch(variant) {
            case 0: code[12]=32;break; // unequal counts
            case 1: code[18]=0xf8;break; // arithmetic right shift
            case 2: code[10]=0xc2;break; // observable intermediate in RDX
            case 3: code[7]=0xe4;code[18]=0xec;break; // RSP
            case 4: code[7]=0xe1;code[18]=0xe9;break; // RCX aliases the shift count
            case 5: code.push_back(0x9c);code.push_back(0x59);break; // PUSHF
            case 6: code.insert(code.end(),{0x0f,0x90,0xc1});break; // SETO
            case 7: code.insert(code.end(),{0x48,0x11,0xc0});break; // ADC
        }
        code.insert(code.end(),{0x48,0x39,0xc8,0xc3});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).masked==0);
    }
    for(size_t target:{size_t(5),size_t(8),size_t(11),size_t(16)}) {
        auto code=maskBlock;code.insert(code.end(),{0x48,0x39,0xc8,0xc3});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),int64_t(target)).masked==0);
        code.insert(code.begin(),{0xeb,uint8_t(target)});
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).masked==0);
    }
    auto relocatedMask=maskBlock;relocatedMask.insert(relocatedMask.end(),{0x48,0x39,0xc8,0xc3});
    const size_t originalTarget=relocatedMask.size();
    relocatedMask.insert(relocatedMask.begin(),{0xeb,uint8_t(originalTarget)});
    relocatedMask.insert(relocatedMask.end(),{0x48,0x89,0xc8,0xc3});
    result=tc::simulation_jit::optimize(relocatedMask.data(),relocatedMask.size(),0);
    CHECK(result.masked==1 && result.code[1]<originalTarget);
    Executable maskJumpA(relocatedMask,0),maskJumpB(result.code,size_t(result.entry));
    CHECK(maskJumpA.run && maskJumpB.run);
    CHECK(maskJumpA.run(seed,&flags,0)==maskJumpB.run(seed,&flags,0));
    result=tc::simulation_jit::optimize(relocatedMask.data(),relocatedMask.size(),int64_t(originalTarget+2));
    CHECK(result.masked==1 && result.entry<int64_t(originalTarget+2));
    Executable maskEntry(result.code,size_t(result.entry));
    CHECK(maskEntry.run && maskEntry.run(seed,&flags,0)==seed);
    const std::vector<uint8_t> deadImmediate={0xb8,32,0,0,0,0x48,0x89,0xc8,0x4c,0x01,0xc0,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    // A full register copy can also die before any use of its destination.
    const std::vector<uint8_t> deadCopy={0x48,0x89,0xc8,0x4c,0x89,0xc0,0x48,0x83,0xc0,1,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    result=tc::simulation_jit::optimize(deadCopy.data(),deadCopy.size(),0);
    CHECK(result.removed==1);
    Executable deadCopyA(deadCopy,0),deadCopyB(result.code,size_t(result.entry));CHECK(deadCopyA.run && deadCopyB.run);
    for(unsigned i=0;i<1000;++i) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        uint64_t flagsA=0,flagsB=0;
        CHECK(deadCopyA.run(seed,&flagsA,~seed)==deadCopyB.run(seed,&flagsB,~seed));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    for(const auto& code:{
        std::vector<uint8_t>{0x48,0x89,0xc8,0xb0,0xa5,0x4d,0x89,0xc9,0xc3},
        std::vector<uint8_t>{0x48,0x89,0xc8,0x48,0x89,0x02,0x4c,0x89,0xc0,0x4d,0x89,0xc9,0xc3},
        std::vector<uint8_t>{0x48,0x89,0xc8,0x84,0xc0,0x4c,0x89,0xc0,0x4d,0x89,0xc9,0x9c,0x59,0x48,0x89,0x0a,0xc3}}) {
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);CHECK(result.deadCopies==0);
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        uint64_t flagsA=0,flagsB=0;
        CHECK(a.run(seed,&flagsA,~seed)==b.run(seed,&flagsB,~seed));
        CHECK(flagsA==flagsB);
    }
    const std::vector<uint8_t> copyAddress={0x48,0x89,0xd0,0x4c,0x8b,0x00,0x48,0x89,0xc8,0x4c,0x01,0xc0,0x4d,0x89,0xc9,0xc3};
    result=tc::simulation_jit::optimize(copyAddress.data(),copyAddress.size(),0);CHECK(result.deadCopies==0);
    Executable copyAddressA(copyAddress,0),copyAddressB(result.code,size_t(result.entry));CHECK(copyAddressA.run && copyAddressB.run);
    uint64_t copyAddressValue=0x1122334455667788ULL;
    CHECK(copyAddressA.run(7,&copyAddressValue,0)==copyAddressValue+7);
    CHECK(copyAddressB.run(7,&copyAddressValue,0)==copyAddressValue+7);
    const std::vector<uint8_t> copyKill32={0x48,0x89,0xc8,0xb8,7,0,0,0,0xc3};
    result=tc::simulation_jit::optimize(copyKill32.data(),copyKill32.size(),0);CHECK(result.deadCopies==1);
    Executable copyKill32B(result.code,size_t(result.entry));CHECK(copyKill32B.run && copyKill32B.run(seed,&flags,0)==7);
    for(const auto& code:{
        std::vector<uint8_t>{0x48,0x89,0xc8,0xc3},
        std::vector<uint8_t>{0x48,0x89,0xc8,0x75,0,0x4c,0x89,0xc0,0xc3},
        std::vector<uint8_t>{0x48,0x89,0xc8,0xe8,4,0,0,0,0x4c,0x89,0xc0,0xc3,0xc3}})
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).deadCopies==0);
    auto copyTarget=deadCopy;
    copyTarget.insert(copyTarget.begin()+3,{0x4d,0x89,0xc9});
    result=tc::simulation_jit::optimize(copyTarget.data(),copyTarget.size(),3);CHECK(result.deadCopies==0);
    Executable copyTargetA(copyTarget,3),copyTargetB(result.code,size_t(result.entry));CHECK(copyTargetA.run && copyTargetB.run);
    uint64_t copyTargetFlagsA=0,copyTargetFlagsB=0;
    CHECK(copyTargetA.run(seed,&copyTargetFlagsA,~seed)==copyTargetB.run(seed,&copyTargetFlagsB,~seed));
    CHECK((copyTargetFlagsA&0x8d5)==(copyTargetFlagsB&0x8d5));
    result=tc::simulation_jit::optimize(deadImmediate.data(),deadImmediate.size(),0);
    CHECK(result.removed==1);
    Executable deadImmediateA(deadImmediate,0),deadImmediateB(result.code,size_t(result.entry));
    CHECK(deadImmediateA.run && deadImmediateB.run);
    for(unsigned i=0;i<1000;++i) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        uint64_t flagsA=0,flagsB=0;
        CHECK(deadImmediateA.run(seed,&flagsA,~seed)==deadImmediateB.run(seed,&flagsB,~seed));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    auto alreadyByte=std::vector<uint8_t>{0x0f,0xb6,0xc1};
    alreadyByte.insert(alreadyByte.end(),maskBlock.begin(),maskBlock.end());
    alreadyByte.insert(alreadyByte.end(),{0x48,0x39,0xc8,0xc3});
    result=tc::simulation_jit::optimize(alreadyByte.data(),alreadyByte.size(),0);
    CHECK(result.removed==4);
    Executable alreadyByteA(alreadyByte,0),alreadyByteB(result.code,size_t(result.entry));
    CHECK(alreadyByteA.run && alreadyByteB.run);
    for(unsigned i=0;i<1000;++i) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        CHECK(alreadyByteA.run(seed,&flags,0)==alreadyByteB.run(seed,&flags,0));
    }
    // An unprefixed CH reads RCX; with REX the same encoding reads BPL.
    for(bool rex:{false,true}) {
        std::vector<uint8_t> code={0xb8,0x78,0x56,0x34,0x12,0xb9,0xef,0xbe,0xad,0xde};
        if(rex)code.push_back(0x40);
        code.insert(code.end(),{0x8a,0xc5,0xb9,100,0,0,0,0x4d,0x89,0xc9,0xc3});
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.deadConstants==(rex?1u:0u));
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        CHECK(a.run(0,&flags,0)==b.run(0,&flags,0));
        if(!rex)CHECK(b.run(0,&flags,0)==0x123456be);
    }
    // Partial writes preserve old upper bits, so the earlier full value lives.
    const std::vector<uint8_t> partial={0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x66,0xb8,0xaa,0xbb,0x4d,0x89,0xc9,0xc3};
    result=tc::simulation_jit::optimize(partial.data(),partial.size(),0);
    CHECK(result.deadConstants==0);
    Executable partialA(partial,0),partialB(result.code,size_t(result.entry));CHECK(partialA.run && partialB.run);
    CHECK(partialA.run(0,&flags,0)==partialB.run(0,&flags,0));
    CHECK(partialB.run(0,&flags,0)==0x112233445566bbaaULL);
    uint64_t table[2]={0x1122334455667788ULL,0xfedcba9876543210ULL};
    for(bool extendedIndex:{false,true}) {
        std::vector<uint8_t> code;
        if(extendedIndex)code={0x41,0xb9,1,0,0,0};
        code.insert(code.end(),{0xb9,1,0,0,0,extendedIndex?uint8_t(0x4a):uint8_t(0x48),0x8b,0x04,0xca,0xb9,0,0,0,0,0x4d,0x89,0xc0,0xc3});
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.deadConstants==(extendedIndex?1u:0u));
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        CHECK(a.run(0,table,0)==table[1] && b.run(0,table,0)==table[1]);
    }
    auto addressBase=std::vector<uint8_t>{0x48,0xb9,0,0,0,0,0,0,0,0};
    const uint64_t tableAddress=reinterpret_cast<uint64_t>(table);memcpy(addressBase.data()+2,&tableAddress,8);
    addressBase.insert(addressBase.end(),{0x4a,0x8b,0x04,0xc1,0xb9,0,0,0,0,0x4d,0x89,0xc9,0xc3}); // [RCX+R8*8]
    result=tc::simulation_jit::optimize(addressBase.data(),addressBase.size(),0);CHECK(result.deadConstants==0);
    Executable baseA(addressBase,0),baseB(result.code,size_t(result.entry));CHECK(baseA.run && baseB.run);
    CHECK(baseA.run(0,&flags,1)==table[1] && baseB.run(0,&flags,1)==table[1]);
    // Calls/returns/branches and merged entries block register-flow proofs.
    for(const auto& code:{
        std::vector<uint8_t>{0xb8,1,0,0,0,0xc3},
        std::vector<uint8_t>{0xb8,1,0,0,0,0x75,0,0x48,0x89,0xc8,0xc3},
        std::vector<uint8_t>{0xb8,1,0,0,0,0xe8,4,0,0,0,0x48,0x89,0xc8,0xc3,0xc3}})
        CHECK(tc::simulation_jit::optimize(code.data(),code.size(),0).deadConstants==0);
    auto mergedByte=std::vector<uint8_t>{0x0f,0xb6,0xc1,0x75,0};
    mergedByte.insert(mergedByte.end(),maskBlock.begin(),maskBlock.end());mergedByte.insert(mergedByte.end(),{0x48,0x39,0xc8,0xc3});
    result=tc::simulation_jit::optimize(mergedByte.data(),mergedByte.size(),0);CHECK(result.redundantMasks==0);
    // Keep full-register zero initialization before a byte Boolean write.
    // Partial SETcc writes must preserve flags and never count as full kills.
    const std::vector<uint8_t> byteBoolean={0x0f,0xb6,0xc1,0x3c,0,0xb8,0,0,0,0,0x0f,0x95,0xc0,0x4d,0x89,0xc9,0x9c,0x59,0x48,0x89,0x0a,0xc3};
    result=tc::simulation_jit::optimize(byteBoolean.data(),byteBoolean.size(),0);
    CHECK(result.removed==1);
    CHECK(result.deadConstants==0);
    Executable byteBooleanA(byteBoolean,0),byteBooleanB(result.code,size_t(result.entry));CHECK(byteBooleanA.run && byteBooleanB.run);
    for(uint64_t input:{0ULL,1ULL,128ULL,255ULL,256ULL,0xffffffffffffffffULL}) {
        uint64_t flagsA=0,flagsB=0;
        CHECK(byteBooleanA.run(input,&flagsA,0)==byteBooleanB.run(input,&flagsB,0));
        CHECK((flagsA&0x8d5)==(flagsB&0x8d5));
    }
    // A known nonzero upper byte, an unknown input or a merged entry must
    // retain the full zero initializer before a partial write.
    for(const auto& prefix:{std::vector<uint8_t>{0xb8,0,1,0,0},
                            std::vector<uint8_t>{0x48,0x89,0xc8},
                            std::vector<uint8_t>{0x0f,0xb6,0xc1,0x75,0}}) {
        auto code=prefix;
        code.insert(code.end(),{0x85,0xc0,0xb8,0,0,0,0,0x0f,0x95,0xc0,0x4d,0x89,0xc9,0xc3});
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.deadConstants==0);
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        CHECK(a.run(0x1122334455667788ULL,&flags,0)==b.run(0x1122334455667788ULL,&flags,0));
    }
    // High-byte and extended-byte immediates overwrite only their own byte.
    for(const auto& code:{
        std::vector<uint8_t>{0xb8,0,0xaa,0,0,0x85,0xc0,0xb8,0,0,0,0,0xb4,0x55,0x4d,0x89,0xc9,0xc3},
        std::vector<uint8_t>{0x41,0xb8,0xff,0,0,0,0x45,0x85,0xc0,0x41,0xb8,0,0,0,0,0x41,0xb0,0x55,0x4c,0x89,0xc0,0x4d,0x89,0xc9,0xc3}}) {
        result=tc::simulation_jit::optimize(code.data(),code.size(),0);
        CHECK(result.removed==1 && result.deadConstants==0);
        Executable a(code,0),b(result.code,size_t(result.entry));CHECK(a.run && b.run);
        CHECK(a.run(0,&flags,0)==b.run(0,&flags,0));
    }
    // The same high-byte write cannot excuse nonzero low bytes.
    const std::vector<uint8_t> nonzeroLow={0xb8,0xaa,0,0,0,0x85,0xc0,0xb8,0,0,0,0,0xb4,0x55,0x4d,0x89,0xc9,0xc3};
    result=tc::simulation_jit::optimize(nonzeroLow.data(),nonzeroLow.size(),0);
    CHECK(result.deadConstants==0);
    Executable lowA(nonzeroLow,0),lowB(result.code,size_t(result.entry));CHECK(lowA.run && lowB.run);
    CHECK(lowA.run(0,&flags,0)==0x5500 && lowB.run(0,&flags,0)==0x5500);
    std::cout<<"PASS simulation JIT: live x64 values/flags, inverse copies, constant shifts/masks, register flow, control-flow and entry relocation\n";
    if(argc==4) {
        std::ifstream input(argv[1],std::ios::binary);
        CHECK(input.good());
        const std::vector<uint8_t> captured{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        result=tc::simulation_jit::optimize(captured.data(),captured.size(),std::stoll(argv[2]));
        std::cout<<"CAPTURE bytes="<<captured.size()<<" removed="<<result.removed<<" specialized="<<result.specialized<<" masked="<<result.masked<<" dead_constants="<<result.deadConstants<<" dead_copies="<<result.deadCopies<<" redundant_masks="<<result.redundantMasks<<" fallthrough_jumps="<<result.fallthroughJumps<<" stack_forwards="<<result.stackForwards<<" equivalent_copies="<<result.equivalentCopies<<" signed_extensions="<<result.signedExtensions<<" unused_extensions="<<result.unusedExtensions<<" unused_masks="<<result.unusedMasks<<" flow_copies="<<result.flowCopies<<" redundant_stores="<<result.redundantStores<<" new_bytes="<<result.code.size()<<" entry="<<result.entry<<'\n';
        CHECK(result.removed || result.specialized);
        std::ofstream output(argv[3],std::ios::binary);output.write(reinterpret_cast<const char*>(result.code.data()),std::streamsize(result.code.size()));
        CHECK(output.good());
    }
}
