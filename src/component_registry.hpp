#pragma once
/* The component catalogue behind TC_SERVICE_COMPONENT_REGISTRY.

   Registration itself is unchanged: a Mod calls register_logic (an imported
   definition whose gate behaviour is replaced by a callback) or
   register_component (a declarative definition the loader turns into a
   scaffold), and the loader then validates the shape against the prototype the
   game compiled.  This catalogue records what happened - including the
   refusals, with the reason the loader rejected them - so a Mod author can ask
   the session what types exist instead of guessing from the editor.

   The store keeps plain C++ values only: the game objects a registration walks
   (prototypes, pin descriptors) are read by the caller and handed over as
   widths and names, which is also what makes the unit test able to drive this
   file without a game. */

#include "../sdk/tc_service_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tc::component_registry {

/* Bounded on purpose: a session with a thousand component types is a bug or an
   attack, and the service reports the cap instead of growing without limit. */
inline constexpr std::size_t kMaxTypes=256;
/* Pins reported per direction.  The definition API itself allows up to eight
   today; the catalogue keeps room for the variable-length stage. */
inline constexpr std::size_t kMaxPinsPerDirection=32;

struct Pin {
    std::string name;
    uint64_t bits=1;
};

struct Type {
    uint64_t custom_id=0;
    std::string owner_mod;
    std::string name;
    std::string description;
    uint64_t gate_cost=0;
    uint64_t delay=0;
    /* The board-cell lane this type's generated pins sit on; the default is
       what every declarative type produced before the pin-lane cut (see
       sdk/tc_service_api.h, TCComponentTypeDefinitionV2::pin_lane). */
    float pin_lane=2.0f;
    uint32_t implementation=TC_COMPONENT_IMPL_NATIVE_CALLBACK;
    uint32_t schema_version=1;
    bool active=false;
    std::string status;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    uint32_t capabilities=0;
};

/* A copy with the derived capability bits filled in and the pin lists capped.
   Everything that enters the catalogue goes through here, so the query side
   never has to recompute them. */
inline Type prepared(Type type) {
    if(type.inputs.size()>kMaxPinsPerDirection)type.inputs.resize(kMaxPinsPerDirection);
    if(type.outputs.size()>kMaxPinsPerDirection)type.outputs.resize(kMaxPinsPerDirection);
    type.capabilities=0;
    if(type.active&&type.implementation==TC_COMPONENT_IMPL_NATIVE_CALLBACK)
        type.capabilities|=TC_COMPONENT_CAP_LOGIC;
    const auto wide=[](const std::vector<Pin>& pins){
        for(const auto& pin:pins)if(pin.bits>1)return true;
        return false;
    };
    if(wide(type.inputs)||wide(type.outputs))type.capabilities|=TC_COMPONENT_CAP_WIDE_PIN;
    if(type.inputs.size()>1||type.outputs.size()>1)type.capabilities|=TC_COMPONENT_CAP_MULTI_PIN;
    return type;
}

inline std::mutex mutex;
inline std::vector<Type> entries;

inline Type* findLocked(uint64_t custom_id) {
    for(auto& entry:entries)if(entry.custom_id==custom_id)return &entry;
    return nullptr;
}

/* Inserts or replaces the entry for this id.  Replacing is what makes a refused
   registration followed by a successful one read as one type. */
inline bool note(Type type) {
    if(!type.custom_id)return false;
    std::lock_guard<std::mutex> lock(mutex);
    Type* existing=findLocked(type.custom_id);
    if(existing){
        if(type.inputs.size()!=existing->inputs.size())existing->inputs.resize(type.inputs.size());
        if(type.outputs.size()!=existing->outputs.size())existing->outputs.resize(type.outputs.size());
        for(std::size_t i=0;i<type.inputs.size();++i)
            if(type.inputs[i].name.empty())type.inputs[i].name=existing->inputs[i].name;
        for(std::size_t i=0;i<type.outputs.size();++i)
            if(type.outputs[i].name.empty())type.outputs[i].name=existing->outputs[i].name;
        *existing=prepared(std::move(type));
        return true;
    }
    if(entries.size()>=kMaxTypes)return false;
    entries.push_back(prepared(std::move(type)));
    return true;
}

/* Records a refusal.  The first reason wins: the bridge explains exactly which
   shape it refused, and a later, vaguer step must not hide it. */
inline void noteRefused(uint64_t custom_id,const std::string& owner_mod,const std::string& name,
                        const std::string& why) {
    if(!custom_id)return;
    std::lock_guard<std::mutex> lock(mutex);
    if(Type* existing=findLocked(custom_id)){
        if(existing->active)return;      /* already registered: nothing to record */
        if(!existing->status.empty())return;
        if(existing->owner_mod.empty())existing->owner_mod=owner_mod;
        if(existing->name.empty())existing->name=name;
        existing->status=why;
        return;
    }
    if(entries.size()>=kMaxTypes)return;
    Type type;
    type.custom_id=custom_id;
    type.owner_mod=owner_mod;
    type.name=name;
    type.active=false;
    type.status=why;
    entries.push_back(prepared(std::move(type)));
}

/* The shape the game compiled.  A registration that only had a callback (an
   imported definition bridged by register_logic) gets its entry here; a
   declarative one already has its names and costs and only has its widths
   confirmed. */
inline void noteShape(uint64_t custom_id,const std::string& owner_mod,
                      const std::vector<Pin>& inputs,const std::vector<Pin>& outputs) {
    if(!custom_id)return;
    std::lock_guard<std::mutex> lock(mutex);
    Type* existing=findLocked(custom_id);
    if(!existing){
        if(entries.size()>=kMaxTypes)return;
        Type fresh;
        fresh.custom_id=custom_id;
        fresh.owner_mod=owner_mod;
        fresh.active=true;
        entries.push_back(fresh);
        existing=&entries.back();
    }
    if(existing->inputs.size()!=inputs.size())existing->inputs.resize(inputs.size());
    for(std::size_t i=0;i<inputs.size();++i){
        existing->inputs[i].bits=inputs[i].bits;
        if(!inputs[i].name.empty())existing->inputs[i].name=inputs[i].name;
    }
    if(existing->outputs.size()!=outputs.size())existing->outputs.resize(outputs.size());
    for(std::size_t i=0;i<outputs.size();++i){
        existing->outputs[i].bits=outputs[i].bits;
        if(!outputs[i].name.empty())existing->outputs[i].name=outputs[i].name;
    }
    *existing=prepared(std::move(*existing));
}

inline void clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}

/* A rejected Mod leaves nothing behind, the catalogue included: its types are
   dropped, and a type another Mod owns is left alone. */
inline void dropMod(const std::string& owner_mod) {
    if(owner_mod.empty())return;
    std::lock_guard<std::mutex> lock(mutex);
    entries.erase(std::remove_if(entries.begin(),entries.end(),
                                 [&](const Type& entry){return entry.owner_mod==owner_mod;}),
                  entries.end());
}

inline uint32_t count() {
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<uint32_t>(entries.size());
}

inline std::vector<Type> snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return entries;
}

inline bool find(uint64_t custom_id,Type* out) {
    std::lock_guard<std::mutex> lock(mutex);
    Type* entry=findLocked(custom_id);
    if(!entry)return false;
    *out=*entry;
    return true;
}

/* The Mod-namespaced spelling of the identity, which is what the ABI reports as
   `type_id`.  A declared string id replaces it in the type-definition stage. */
inline std::string typeId(const Type& type) {
    char hex[32];
    std::snprintf(hex,sizeof(hex),"0x%llx",static_cast<unsigned long long>(type.custom_id));
    return (type.owner_mod.empty()?std::string("(unknown)"):type.owner_mod)+"/"+hex;
}

/* Copies `text` into a fixed-size ABI field, always NUL terminated. */
inline void copyField(char* destination,std::size_t capacity,const std::string& text) {
    if(!destination||!capacity)return;
    const std::size_t copied=text.size()<capacity-1?text.size():capacity-1;
    std::memcpy(destination,text.data(),copied);
    destination[copied]=0;
}

}  // namespace tc::component_registry
