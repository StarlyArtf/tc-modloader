#pragma once
#include "component_timing_graph.hpp"
#include "../sdk/tc_game_model.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace tc::timing {
using Preorder = void(*)(void*,void*,void*,void*,void*,void*,uint64_t,void*);
using SetPrototype = void(*)(uint64_t,const void*);
using Cost = void*(*)(void*,const void*);
inline Preorder originalPreorder=nullptr;
inline SetPrototype originalSet=nullptr;
inline Cost getCost=nullptr;
inline std::mutex registryMutex;
inline std::unordered_set<uint64_t> modIds;
inline thread_local std::vector<uint64_t>* registering=nullptr;
struct Registration {
    std::vector<uint64_t> ids;
    Registration(){registering=&ids;}
    ~Registration(){registering=nullptr;}
};
inline std::function<void(const std::string&)> timingLog;
inline void (*getPrototype)(uint64_t,void*)=nullptr;
inline void (*destroyPrototype)(void*)=nullptr;
struct Compile {
    bool attempted=false, ready=false;
    uintptr_t data=0;
    std::vector<int64_t> desired;
};
inline thread_local Compile* active=nullptr;
inline uint64_t read64(const void* p,size_t offset) {
    uint64_t v; memcpy(&v,static_cast<const char*>(p)+offset,8); return v;
}
inline void setPrototype(uint64_t id,const void* prototype) {
    originalSet(id,prototype);
    if(registering) registering->push_back(id);
}
inline void commit(const std::vector<uint64_t>& ids) {
    std::lock_guard<std::mutex> lock(registryMutex);
    modIds.insert(ids.begin(),ids.end());
}
inline void prepare(Compile& state,const void* graph) {
    state.attempted=true;
    std::unordered_set<uint64_t> ids;
    {std::lock_guard<std::mutex> lock(registryMutex); ids=modIds;}
    if(ids.empty()) return;
    auto count=read64(graph,0x60), data=read64(graph,0x68), nets=read64(graph,0x128);
    if(!data || count>1000000 || nets>4000000 ||
       read64(graph,0x108)!=count || read64(graph,0x180)!=count) return;
    auto component=[&](size_t i){return reinterpret_cast<const void*>(data+8+i*0x238);};
    std::unordered_map<uint64_t,size_t> custom;
    std::unordered_map<size_t,int64_t> declared;
    for(size_t i=0;i<count;++i) {
        auto c=component(i);
        if(*static_cast<const uint8_t*>(c)!=0x4e) continue;
        if(!custom.emplace(read64(c,8),i).second) return;
        auto id=read64(c,0x188);
        if(!ids.count(id)) continue;
        TCPrototype p{}; getPrototype(id,&p);
        auto delay=prototypeDelay(p); destroyPrototype(&p);
        if(delay>uint64_t(INT64_MAX)) return;
        declared.emplace(i,static_cast<int64_t>(delay));
    }
    if(declared.empty()) return;
    std::vector<Node> nodes(count);
    std::vector<int64_t> groupDelays;
    std::unordered_map<size_t,int> groupIndex;
    for(size_t i=0;i<count;++i) {
        auto c=component(i);
        uint64_t path=read64(c,0x18);
        size_t root=SIZE_MAX;
        std::unordered_set<uint64_t> seen;
        while(path) {
            if(!seen.insert(path).second) return;
            auto it=custom.find(path);
            if(it==custom.end()) return;
            if(declared.count(it->second)) root=it->second;
            path=read64(component(it->second),0x10);
        }
        if(root!=SIZE_MAX) {
            auto [it,inserted]=groupIndex.emplace(root,int(groupDelays.size()));
            if(inserted) groupDelays.push_back(declared.at(root));
            nodes[i].group=it->second;
        }
        int64_t pair[2]{}; getCost(pair,c); nodes[i].delay=pair[1];
        for(size_t offset:{size_t(0x108),size_t(0x180)}) {
            auto entries=read64(graph,offset+8);
            if(!entries) return;
            auto entry=reinterpret_cast<const void*>(entries+8+i*16);
            auto length=read64(entry,0), payload=read64(entry,8);
            if(length>65536 || (length && !payload)) return;
            auto& list=offset==0x108?nodes[i].inputs:nodes[i].outputs;
            for(size_t j=0;j<length;++j) list.push_back(read64(reinterpret_cast<void*>(payload),8+j*8));
        }
    }
    const auto result=solve(nodes,groupDelays,nets);
    if(!result.supported) {
        if(timingLog) timingLog("Component timing: native timing retained for feedback/multiple-driver/unsupported graph");
        return;
    }
    state.data=data+8;
    state.desired.assign(count,-1);
    for(size_t i=0;i<count;++i) if(nodes[i].group>=0) state.desired[i]=result.arrival[i];
    state.ready=true;
}
inline void preorder(void* a,void* b,void* c,void* d,void* e,void* f,uint64_t g,void* out) {
    Compile state;
    struct Restore { Compile* previous; ~Restore(){active=previous;} } restore{active};
    active=&state;
    originalPreorder(a,b,c,d,e,f,g,out);
}
}

// This shim replaces ONLY the pinned preorder timing call site. r14 is the
// compile graph and rsi the already computed maximum input arrival there.
// Other get_cost callers (including menus and gate counting) remain intact.
extern "C" __attribute__((noinline,used)) inline void* tc_timing_cost(
    void* out,const void* component,const void* graph,int64_t incoming) {
    using namespace tc::timing;
    getCost(out,component);
    if(!active) return out;
    try {
        if(!active->attempted) prepare(*active,graph);
        const auto address=reinterpret_cast<uintptr_t>(component);
        if(active->ready && address>=active->data && (address-active->data)%0x238==0) {
            const auto i=(address-active->data)/0x238;
            if(i<active->desired.size() && incoming>=0 && active->desired[i]>=incoming) {
                const auto delay=active->desired[i]-incoming;
                memcpy(static_cast<char*>(out)+8,&delay,8);
            }
        }
    } catch(...) { active->ready=false; }
    return out;
}
extern "C" __attribute__((naked,used)) inline void* tc_timing_shim(void*,const void*) {
    __asm__ volatile("mov %r14, %r8\nmov %rsi, %r9\njmp tc_timing_cost");
}

namespace tc::timing {
inline bool install(const std::map<std::string,void*>& symbols,
                    std::set<void*>& owned,
                    const std::function<void(const std::string&)>& log) {
    auto resolve=[&](const char* s)->void* {auto i=symbols.find(s);return i==symbols.end()?nullptr:i->second;};
    auto p=resolve("preorder__modelZsimulationZpreorder_u8749");
    auto s=resolve("custom_prototypes_set__modelZboardZcustom95prototype95list_u192");
    getCost=reinterpret_cast<Cost>(resolve("get_cost__modelZscores_u2321"));
    getPrototype=reinterpret_cast<void(*)(uint64_t,void*)>(resolve("get_custom_prototype__modelZboardZcustom95prototype95list_u451"));
    destroyPrototype=reinterpret_cast<void(*)(void*)>(resolve("eqdestroy___modelZboardZprototype95list_u3259"));
    if(!p || !s || !getCost || !getPrototype || !destroyPrototype) return false;
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto site=reinterpret_cast<unsigned char*>(base+0x184905);
    const unsigned char expected[]={0xe8,0x56,0x44,0xfd,0xff};
    if(memcmp(site,expected,sizeof(expected))) throw std::runtime_error("Component timing call-site mismatch");
    unsigned char* thunk=nullptr;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    for(uintptr_t delta=info.dwAllocationGranularity;delta<0x70000000;delta+=info.dwAllocationGranularity) {
        thunk=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>((base+delta)&~(uintptr_t(info.dwAllocationGranularity)-1)),4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(thunk) break;
    }
    if(!thunk) throw std::runtime_error("Component timing thunk allocation failed");
    // Absolute indirect jump, no register clobbering.
    const unsigned char jump[]={0xff,0x25,0,0,0,0}; memcpy(thunk,jump,6);
    auto destination=&tc_timing_shim; memcpy(thunk+6,&destination,8);
    DWORD protection;
    if(!VirtualProtect(thunk,4096,PAGE_EXECUTE_READ,&protection)) { VirtualFree(thunk,0,MEM_RELEASE); throw std::runtime_error("Component timing thunk protection failed"); }
    FlushInstructionCache(GetCurrentProcess(),thunk,14);
    bool hp=false,hs=false;
    if(MH_CreateHook(p,reinterpret_cast<void*>(&preorder),reinterpret_cast<void**>(&originalPreorder))==MH_OK) hp=true;
    if(hp && MH_CreateHook(s,reinterpret_cast<void*>(&setPrototype),reinterpret_cast<void**>(&originalSet))==MH_OK) hs=true;
    if(!hs || MH_EnableHook(p)!=MH_OK || MH_EnableHook(s)!=MH_OK ||
       !VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&protection)) {
        if(hp){MH_DisableHook(p);MH_RemoveHook(p);} if(hs){MH_DisableHook(s);MH_RemoveHook(s);}
        VirtualFree(thunk,0,MEM_RELEASE); throw std::runtime_error("Component timing hook installation failed");
    }
    const auto relative=static_cast<int32_t>(reinterpret_cast<intptr_t>(thunk)-reinterpret_cast<intptr_t>(site+5));
    memcpy(site+1,&relative,4);
    DWORD ignored; VirtualProtect(site,5,protection,&ignored);
    FlushInstructionCache(GetCurrentProcess(),site,5);
    owned.insert(p);owned.insert(s);timingLog=log;
    log("Component timing: Mod prototype delays enabled in compiled critical paths");
    return true;
}
}
