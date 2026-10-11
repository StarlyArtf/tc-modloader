#pragma once
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_game_model.h"
#include "component_container.hpp"
#include "component_registry.hpp"
#include "keyboard_fix.hpp"
#include "simulation_jit.hpp"
#include "rv32_intrinsic.hpp"
#include "scope_capture.hpp"
#include "../vendor/minhook/include/MinHook.h"   /* hooking the code generator */
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "component_tail.hpp"

namespace tc::logic {
// Every foreign call keeps the verified four-argument shape.  Inputs wider than
// the invoke call's two payload words are staged in 128-bit chunks first.
/* A V2 definition (tc.component.types) may declare more pins than the V1
   callback struct can carry; the bridge's own arrays are sized for the larger
   shape, and `supportedScaffold` keeps V1 definitions at eight. */
inline constexpr uint32_t kMaxBridgePins=32;
inline constexpr uint32_t kMaxBridgeInputs=kMaxBridgePins;
inline constexpr uint32_t kMaxBridgeOutputs=kMaxBridgePins;
inline constexpr uint32_t kMaxPinBits=64;
inline constexpr uint32_t kMaxInputBits=2048;
inline constexpr uint32_t kInputWordBits=64;
inline constexpr uint32_t kInputWordsPerCall=2;
inline constexpr uint32_t kMaxInputWords=kMaxInputBits/kInputWordBits;
inline constexpr uint32_t kMaxInputChunks=kMaxInputWords/kInputWordsPerCall;
inline constexpr uint32_t kMaxConfigBytes=64u*1024u;
/* Private TC-Verilog selector: run the CYCLE/edge logic but do not evaluate a
   visible output cone afterwards. */
inline constexpr uint32_t kTickOnlySelector=~uint32_t(0);
inline constexpr uint32_t kOutputGroupSelector=uint32_t(1)<<31;
struct Definition {
    TCLogicDefinition api;
    uint32_t inputs=0,outputs=0;
    std::array<uint32_t,kMaxBridgeInputs> inputWidths{};
    std::array<uint32_t,kMaxBridgeOutputs> outputWidths{};
    // Payload word index and bit offset of each input inside the invoke call.
    std::array<uint32_t,kMaxBridgeInputs> inputWord{};
    std::array<uint32_t,kMaxBridgeInputs> inputShift{};
    uint32_t inputBits=0;
    /* Empty keeps the historical all-input dependency chain.  Otherwise one
       mask per output describes the real combinational cone. */
    std::vector<uint32_t> outputDependencies;
    /* One hidden all-input sink runs the sequential CYCLE callback.  It has no
       public output, so it cannot participate in a combinational feedback
       path; visible precise drivers evaluate the callback in REFRESH only. */
    bool clockedTick=false;
    bool groupedOutputs=false;
    std::vector<std::string> inlinePrefixes;
    std::vector<std::string> inlineExpressions;
    bool wideOutputs=false;
    bool active=false;
    /* V2/V3 (tc.component.types): callbacks take borrowed arrays and the
       per-instance state size comes from the definition. */
    bool useV2=false;
    bool useV3=false;
    TCLogicCallbackV2 callbackV2=nullptr;
    TCLogicCallbackV3 callbackV3=nullptr;
    uint32_t stateWords=8;
    const TCComponentLifecycleV1* lifecycle=nullptr;
    uint32_t configSchema=0;
    std::vector<uint8_t> defaultConfig;
    /* A definition may convert a configuration saved under an older schema; a
       definition without one keeps whatever the circuit carries and runs on its
       default configuration instead. */
    uint32_t migrationVersion=0;
    TCComponentConfigMigration migrateConfig=nullptr;
    void* migrationUser=nullptr;
};
// Pack contiguously, including pins crossing a 64-bit word boundary.
inline void placeInputs(const std::array<uint32_t,kMaxBridgeInputs>& widths,uint32_t count,
                        std::array<uint32_t,kMaxBridgeInputs>& word,
                        std::array<uint32_t,kMaxBridgeInputs>& shift) {
    uint32_t currentWord=0,currentShift=0;
    for(uint32_t i=0;i<count;++i) {
        const uint32_t bits=widths[i];
        word[i]=currentWord;shift[i]=currentShift;currentShift+=bits;
        currentWord+=currentShift/64;currentShift%=64;
    }
}
struct Binding {
    struct InputStage {
        std::array<uint64_t,kMaxInputWords> values{};
        std::array<uint64_t,kMaxInputWords> drives{};
        uint64_t valueCycle=0,driveCycle=0;
        uint32_t valueChunks=0,driveChunks=0;
        bool valueCycleValid=false,driveCycleValid=false;
    };
    std::shared_ptr<Definition> definition;
    uint64_t instance=0;
    /* The id the game's selection set and its own top-level lookups address
       this component by: the component's own id when it sits directly on the
       board, otherwise the id of the custom component that contains it.  Filled
       by the same compile walk that used to publish interactive-map.txt, and
       what tc.component.instances V2 hands back. */
    uint64_t topLevel=0;
    /* Host-issued generation: a handle from before the slot was rebound is
       refused instead of resolving to whatever lives there now. */
    uint64_t generation=0;
    uint64_t calls=0,peeks=0,resets=0;
    bool created=false;
    /* Where the instance sits, as the host last read it from the board record.
       The first reading only seeds these fields (being compiled is not a move);
       a later change is what TC_LOGIC_MOVE / TC_LOGIC_ROTATE announce. */
    int32_t cellX=0,cellY=0;
    uint32_t rotation=0;
    bool geometryKnown=false;
    /* The footprint the host last announced, in board cells (half extents).  A
       change is TC_LOGIC_RESIZE. */
    float halfWidth=0.f,halfHeight=0.f;
    bool sizeKnown=false;
    /* The host created this instance by duplicating another one: its first bind
       announces that through TC_LOGIC_CLONE after on_create. */
    bool clonePending=false;
    /* The configuration in this binding was installed from the circuit's record
       (rather than being the registered default): the first bind says so through
       TC_LOGIC_LOAD. */
    bool loadedFromRecord=false;
    /* A released instance keeps its slot so the tokens of the others do not
       move; the slot is reused by the next bind and gets a fresh generation. */
    bool active=true;
    /* `state` is sized by the definition (V1 definitions keep the historical
       eight words); `outputs` holds one word per output pin. */
    std::vector<uint64_t> state;
    std::vector<uint64_t> refreshState;
    std::vector<uint8_t> config;
    uint64_t configRevision=1;
    /* Set when the instance's component record owns a tail table this build can
       address, and when that table already carries a record for this instance.
       `tailRecord` is what makes an unchanged write skippable; a failed commit
       clears it, so the next write repairs the record. */
    bool tailBound=false;
    bool tailRecord=false;
    /* Latched once the record has actually been looked for, so the retry on the
       next service call never overwrites a configuration a plugin has already
       replaced. */
    bool tailProbed=false;
    /* An open configuration edit transaction (tc.component.storage V2): the
       configuration the instance had when the tool said where its action began. */
    bool editOpen=false;
    std::vector<uint8_t> editBefore;
    std::array<uint64_t,kMaxBridgeOutputs> outputs{};
    /* Borrowed arrays handed to a V2 callback, kept here so a call never
       allocates on the simulation thread. */
    std::array<uint64_t,kMaxBridgeInputs> v2Inputs{};
    std::array<TCLogicValueV3,kMaxBridgeInputs> v3Inputs{};
    std::array<TCLogicValueV3,kMaxBridgeOutputs> v3Outputs{};
    /* The generated program latches the drive plane separately.  The two-word
       fast path fills the first entries; wider V3 inputs use the same chunked
       staging protocol as their value plane. */
    std::array<uint64_t,kMaxInputWords> inputDrive{};
    bool inputDriveReady=false;
    /* CYCLE and REFRESH can be prepared independently.  A selector carried by
       each stage call identifies phase, cycle and chunk, so stale or incomplete
       batches are rejected instead of being mixed into a callback. */
    std::array<InputStage,2> inputStages{};
    bool inputStageWarned=false;
    bool partialDriveWarned=false;
    std::mutex mutex;
};
inline std::mutex registryMutex;
inline std::unordered_map<uint64_t,std::shared_ptr<Definition>> definitions;
inline std::vector<std::shared_ptr<Binding>> bindings;
inline std::map<std::pair<uint64_t,uint64_t>,uint64_t> bindingKeys;
inline std::atomic<uint64_t> instanceGeneration{0};
/* Wide constants are emitted as runtime loads instead of literals.  The
   board keeps owning/persisting the setting; this mirror lets the JIT observe
   a punch-tape edit without recompiling a large circuit. */
inline std::mutex dynamicConstantMutex;
/* Slots are separately allocated and never removed, so a simulation thread may
   cache their address across unordered_map rehashes.  Edits are rare; reads are
   on the generated program's hot path and use the slot address directly.
   The exported lookup keeps its TLS cache for older compiled callers. */
inline std::unordered_map<uint64_t,std::unique_ptr<std::atomic<uint64_t>>> dynamicConstants;
inline std::atomic<uint64_t>* dynamicConstantSlot(uint64_t component,uint64_t initial) {
    std::lock_guard<std::mutex> lock(dynamicConstantMutex);
    auto found=dynamicConstants.find(component);
    if(found!=dynamicConstants.end())return found->second.get();
    auto slot=std::make_unique<std::atomic<uint64_t>>(initial);
    auto* result=slot.get();
    dynamicConstants.emplace(component,std::move(slot));
    return result;
}
inline void setDynamicConstant(uint64_t component,uint64_t value) {
    dynamicConstantSlot(component,value)->store(value,std::memory_order_relaxed);
}
inline uint64_t getDynamicConstant(uint64_t component,uint64_t fallback) {
    thread_local uint64_t lastComponent=~uint64_t(0);
    thread_local std::atomic<uint64_t>* lastSlot=nullptr;
    if(lastSlot&&lastComponent==component)
        return lastSlot->load(std::memory_order_relaxed);
    thread_local std::unordered_map<uint64_t,std::atomic<uint64_t>*> cache;
    auto found=cache.find(component);
    if(found==cache.end())
        found=cache.emplace(component,dynamicConstantSlot(component,fallback)).first;
    lastComponent=component;
    lastSlot=found->second;
    return lastSlot->load(std::memory_order_relaxed);
}
inline std::function<void(const std::string&)> logger;
inline bool installed=false;
/* The generated-source dumps are a development aid with a real cost: every
   compile writes them into the game directory (the live install had 60+ of
   them).  They are therefore opt-in, read once per process. */
inline bool dumpSourceEnabled() {
    static const bool enabled=[]{
        const char* value=std::getenv("TC_MODLOADER_DUMP_SOURCE");
        return value&&value[0]&&value[0]!='0';
    }();
    return enabled;
}

/* Six-argument bridge calls were measured and found unreliable in the game's
   JIT.  The diagnostic entry points remain registered for old development
   captures, but production emission uses the four-argument staged protocol. */
inline void (*getPrototype)(uint64_t,void*)=nullptr;
inline void (*destroyPrototype)(void*)=nullptr;
// Simulation state buffer: foreign call results cannot be consumed as words, so
// word outputs are published here as one byte per bit and the generated program
// loads them back the same way the game's own word components do.
inline unsigned char** stateBuffer=nullptr;
inline constexpr uint64_t kBridgeStateBase=0x9a0000;
/* One byte per output bit: 32 pins x 64 bits.  All 32 token slots end at
   0x9b0000, below the pinned build's 0x9c4000 simulation-state boundary. */
inline constexpr uint64_t kBridgeStateStride=kMaxBridgeOutputs*kMaxPinBits;
inline constexpr uint64_t kBridgeStateTokens=32;
inline uint64_t stateSlot(uint64_t token,uint32_t pin,uint32_t bit) {
    return kBridgeStateBase+(token-1)*kBridgeStateStride+pin*64+bit;
}
// Writes every output bit of an instance into its reserved state slots.
inline void publishWordOutputs(uint64_t token,const TCLogicIO& io) {
    if(!stateBuffer||!*stateBuffer||!token||token>kBridgeStateTokens)return;
    for(uint32_t pin=0;pin<io.output_count&&pin<8;++pin) {
        const uint64_t word=io.outputs[pin];
        for(uint32_t bit=0;bit<64;++bit)
            (*stateBuffer)[stateSlot(token,pin,bit)]=static_cast<unsigned char>((word>>bit)&1);
    }
}
/* Same publication for a V2 output array: word-width pins are assembled bit by
   bit in the generated program, so every pin's bits go into the token's slot. */
inline void publishWordOutputs(uint64_t token,const uint64_t* outputs,uint32_t outputCount) {
    if(!stateBuffer||!*stateBuffer||!token||token>kBridgeStateTokens)return;
    for(uint32_t pin=0;pin<outputCount&&pin<kMaxBridgeOutputs;++pin) {
        const uint64_t word=outputs[pin];
        for(uint32_t bit=0;bit<64;++bit)
            (*stateBuffer)[stateSlot(token,pin,bit)]=static_cast<unsigned char>((word>>bit)&1);
    }
}
inline uint64_t read64(const void* p,size_t n) {uint64_t v;memcpy(&v,static_cast<const char*>(p)+n,8);return v;}
inline void note(const std::string& message) {if(logger)logger("Native logic: "+message);}
inline std::string hex64(uint64_t value) {std::ostringstream text;text<<"0x"<<std::hex<<value<<std::dec;return text.str();}
// A prototype pin descriptor stores its relative connection point as two int16
// values at descriptor+2.  The TCPin* helper returns the 8-byte sequence head,
// so the descriptor itself starts 8 bytes later (see docs/sdk/game-model.md).
struct PinGeometry {bool present=false;int64_t x=0,y=0;uint64_t word=0;};
inline PinGeometry readPinGeometry(const TCPrototype& prototype,bool input,uint64_t index) {
    PinGeometry result;
    TCPin* pin=input?prototypeInputPin(prototype,index):prototypeOutputPin(prototype,index);
    if(!pin)return result;
    const auto* descriptor=reinterpret_cast<const unsigned char*>(pin)+8;
    int16_t x=0,y=0;memcpy(&x,descriptor+2,sizeof(x));memcpy(&y,descriptor+4,sizeof(y));
    result.present=true;result.x=x;result.y=y;result.word=pinWordSizeRaw(*pin);
    return result;
}
inline std::string pinText(const PinGeometry& pin) {
    if(!pin.present)return "(missing)";
    return "("+std::to_string(pin.x)+","+std::to_string(pin.y)+",w"+std::to_string(pin.word)+")";
}
// Shape validation for the imported scaffold; the game keeps generating code
// for every ordinary part of the board around these custom pins.
inline std::string describePrototype(uint64_t custom_id,const TCPrototype& p) {
    const uint64_t inputs=prototypeInputCount(p),outputs=prototypeOutputCount(p);
    std::string text="custom "+hex64(custom_id)+" inputs="+std::to_string(inputs)+
        " outputs="+std::to_string(outputs);
    for(uint64_t i=0;i<inputs&&i<4;++i)text+=" in"+std::to_string(i)+"="+pinText(readPinGeometry(p,true,i));
    for(uint64_t i=0;i<outputs&&i<4;++i)text+=" out"+std::to_string(i)+"="+pinText(readPinGeometry(p,false,i));
    return text;
}
// Shape limits of the bridge: `maxPins` pins per direction (eight for a V1
// definition, thirty-two for V2/V3), each 1..64 bits, with the total input width
// fitting the staged-input batch.
inline bool supportedScaffold(uint64_t custom_id,const TCPrototype& p,std::string& detail,
                              std::array<uint32_t,kMaxBridgeInputs>& inputWidths,
                              std::array<uint32_t,kMaxBridgeOutputs>& outputWidths,
                              uint32_t& inputBits,uint32_t maxInputs=kMaxBridgeInputs,
                              uint32_t maxOutputs=kMaxBridgeOutputs) {
    detail=describePrototype(custom_id,p);
    const uint64_t inputs=prototypeInputCount(p),outputs=prototypeOutputCount(p);
    if(inputs>maxInputs||outputs>maxOutputs)return false;
    inputBits=0;
    for(uint64_t i=0;i<inputs;++i) {
        const auto pin=readPinGeometry(p,true,i);
        if(!pin.present||pin.word<1||pin.word>kMaxPinBits)return false;
        inputWidths[i]=static_cast<uint32_t>(pin.word);
        inputBits+=inputWidths[i];
    }
    if(inputBits>kMaxInputBits)return false;
    for(uint64_t i=0;i<outputs;++i) {
        const auto pin=readPinGeometry(p,false,i);
        if(!pin.present||pin.word<1||pin.word>kMaxPinBits)return false;
        outputWidths[i]=static_cast<uint32_t>(pin.word);
    }
    return true;
}
inline int add(const TCLogicDefinition* d,const char* owner_mod=nullptr) {
    if(!installed)return -1;
    if(!d || d->size!=sizeof(*d) || (d->version!=2 && d->version!=3) || !d->custom_id || !d->callback) {
        note("registration rejected: malformed TCLogicDefinition");
        return -1;
    }
    TCPrototype p{};getPrototype(d->custom_id,&p);
    std::string detail;
    std::array<uint32_t,kMaxBridgeInputs> inputWidths{};
    std::array<uint32_t,kMaxBridgeOutputs> outputWidths{};
    uint32_t inputBits=0;
    const bool valid=supportedScaffold(d->custom_id,p,detail,inputWidths,outputWidths,inputBits,8,8);
    const uint32_t inputs=valid?static_cast<uint32_t>(prototypeInputCount(p)):0;
    const uint32_t outputs=valid?static_cast<uint32_t>(prototypeOutputCount(p)):0;
    destroyPrototype(&p);
    if(!valid) {
        note("registration rejected: "+detail+
             " (supported V1 shape is up to eight pins per direction, each 1..64 bits, total input width <= 512)");
        tc::component_registry::noteRefused(d->custom_id,owner_mod?owner_mod:"","",
            "the bridge supports up to eight pins per direction, each 1..64 bits, "
            "with at most 512 input bits in total; "+detail);
        return -2;
    }
    std::lock_guard<std::mutex> lock(registryMutex);
    if(definitions.count(d->custom_id)) {
        tc::component_registry::noteRefused(d->custom_id,owner_mod?owner_mod:"","",
                                            "that id is already registered");
        return -3;
    }
    auto entry=std::make_shared<Definition>();entry->api=*d;
    entry->inputs=inputs;entry->outputs=outputs;
    entry->inputWidths=inputWidths;entry->outputWidths=outputWidths;entry->inputBits=inputBits;
    for(uint32_t i=0;i<outputs;++i)if(outputWidths[i]>1)entry->wideOutputs=true;
    placeInputs(entry->inputWidths,inputs,entry->inputWord,entry->inputShift);
    definitions.emplace(d->custom_id,std::move(entry));
    note("registered "+detail+" shape="+std::to_string(inputs)+"in/"+std::to_string(outputs)+"out");
    /* The catalogue entry follows the shape the game really compiled, so a
       definition registered only through register_logic (an imported circuit
       whose drivers are bridged) is described as completely as a declarative
       one; a declarative registration has already put its pin names in place. */
    {
        std::vector<tc::component_registry::Pin> inputPins,outputPins;
        for(uint32_t i=0;i<inputs;++i)inputPins.push_back({std::string(),inputWidths[i]});
        for(uint32_t i=0;i<outputs;++i)outputPins.push_back({std::string(),outputWidths[i]});
        tc::component_registry::noteShape(d->custom_id,owner_mod?owner_mod:"",inputPins,outputPins);
    }
    return 0;
}
/* V2 registration (tc.component.types): same import/bridge flow, but the
   callback takes `TCLogicIOV2`, the state size comes from the definition, and
   the shape limit is the bridge's own array size instead of the V1 struct's
   eight pins.  The scaffold the game compiles is identical, so the emission
   path (roles, packing, readback) is shared with V1. */
struct V2Registration {
    uint64_t custom_id=0;
    TCLogicCallbackV2 callback=nullptr;
    void* user=nullptr;
    uint32_t stateWords=0;
    const char* owner_mod=nullptr;
    const TCComponentLifecycleV1* lifecycle=nullptr;
    uint32_t configSchema=0;
    std::vector<uint8_t> defaultConfig;
    uint32_t migrationVersion=0;
    TCComponentConfigMigration migrateConfig=nullptr;
    void* migrationUser=nullptr;
    std::vector<uint32_t> outputDependencies;
    bool clockedTick=false;
    bool groupedOutputs=false;
    std::vector<std::string> inlinePrefixes;
    std::vector<std::string> inlineExpressions;
};
struct V3Registration {
    uint64_t custom_id=0;
    TCLogicCallbackV3 callback=nullptr;
    void* user=nullptr;
    uint32_t stateWords=0;
    const char* owner_mod=nullptr;
    const TCComponentLifecycleV1* lifecycle=nullptr;
    uint32_t configSchema=0;
    std::vector<uint8_t> defaultConfig;
    uint32_t migrationVersion=0;
    TCComponentConfigMigration migrateConfig=nullptr;
    void* migrationUser=nullptr;
    std::vector<uint32_t> outputDependencies;
    bool clockedTick=false;
    bool groupedOutputs=false;
    std::vector<std::string> inlinePrefixes;
    std::vector<std::string> inlineExpressions;
};
inline int addVersioned(const V2Registration& registration,TCLogicCallbackV3 callbackV3) {
    if(!installed)return -1;
    if(!registration.custom_id||(!registration.callback&&!callbackV3)) {
        note("registration rejected: malformed versioned definition");
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    }
    const uint64_t custom_id=registration.custom_id;
    const char* owner_mod=registration.owner_mod;
    TCPrototype p{};getPrototype(custom_id,&p);
    std::string detail;
    std::array<uint32_t,kMaxBridgeInputs> inputWidths{};
    std::array<uint32_t,kMaxBridgeOutputs> outputWidths{};
    uint32_t inputBits=0;
    const bool shapeOk=supportedScaffold(custom_id,p,detail,inputWidths,outputWidths,inputBits,
                                         kMaxBridgeInputs,kMaxBridgeOutputs);
    const uint32_t inputs=shapeOk?static_cast<uint32_t>(prototypeInputCount(p)):0;
    const uint32_t outputs=shapeOk?static_cast<uint32_t>(prototypeOutputCount(p)):0;
    destroyPrototype(&p);
    if(!shapeOk) {
        note("registration rejected: "+detail+
             " (V2/V3 shape is up to thirty-two pins per direction, each 1..64 bits, "
             "total input width <= 2048)");
        tc::component_registry::noteRefused(custom_id,owner_mod?owner_mod:"","",
            "the bridge supports up to thirty-two pins per direction, each 1..64 bits, "
            "with at most 2048 input bits in total; "+detail);
        return TC_COMPONENT_TYPES_ERR_UNSUPPORTED;
    }
    std::lock_guard<std::mutex> lock(registryMutex);
    if(definitions.count(custom_id)) {
        tc::component_registry::noteRefused(custom_id,owner_mod?owner_mod:"","",
                                            "that id is already registered");
        return TC_COMPONENT_TYPES_ERR_DUPLICATE;
    }
    auto entry=std::make_shared<Definition>();
    entry->api.size=sizeof(TCLogicDefinition);
    entry->api.version=3;              /* generated scaffolding, like V1 declarative */
    entry->api.custom_id=custom_id;
    entry->api.callback=nullptr;
    entry->api.user=registration.user;
    entry->useV2=callbackV3==nullptr;
    entry->useV3=callbackV3!=nullptr;
    entry->callbackV2=registration.callback;
    entry->callbackV3=callbackV3;
    entry->stateWords=registration.stateWords;
    entry->lifecycle=registration.lifecycle;
    entry->configSchema=registration.configSchema;
    entry->defaultConfig=registration.defaultConfig;
    entry->migrationVersion=registration.migrationVersion;
    entry->migrateConfig=registration.migrateConfig;
    entry->migrationUser=registration.migrationUser;
    entry->inputs=inputs;entry->outputs=outputs;
    if(registration.outputDependencies.size()==outputs)
        entry->outputDependencies=registration.outputDependencies;
    entry->clockedTick=registration.clockedTick&&!entry->outputDependencies.empty();
    entry->groupedOutputs=registration.groupedOutputs&&!entry->outputDependencies.empty()&&
                          callbackV3!=nullptr;
    if(registration.inlineExpressions.size()==outputs) {
        entry->inlinePrefixes=registration.inlinePrefixes;
        entry->inlineExpressions=registration.inlineExpressions;
        entry->groupedOutputs=false;
    }
    entry->inputWidths=inputWidths;entry->outputWidths=outputWidths;entry->inputBits=inputBits;
    for(uint32_t i=0;i<outputs;++i)if(outputWidths[i]>1)entry->wideOutputs=true;
    placeInputs(entry->inputWidths,inputs,entry->inputWord,entry->inputShift);
    definitions.emplace(custom_id,std::move(entry));
    std::string registrationNote="registered "+detail+" shape="+std::to_string(inputs)+"in/"+
        std::to_string(outputs)+"out "+(callbackV3?"v3":"v2")+" state="+
        std::to_string(registration.stateWords);
    /* The exact dependency map is what the game's cycle check sees, so print it
       with the rest of the shape: a false "combinational loop" is diagnosed by
       comparing these masks with the wires a board builds around the part. */
    if(outputs&&registration.outputDependencies.size()==outputs) {
        registrationNote+=" deps=";
        for(uint32_t i=0;i<outputs;++i) {
            if(i)registrationNote+=",";
            char mask[16];
            std::snprintf(mask,sizeof(mask),"0x%x",registration.outputDependencies[i]);
            registrationNote+=mask;
        }
        if(registration.clockedTick)registrationNote+=" tick=hidden";
        if(registration.groupedOutputs&&callbackV3)registrationNote+=" groups=shared-mask";
        if(registration.inlineExpressions.size()==outputs)registrationNote+=" jit=inline";
    }
    note(registrationNote);
    {
        std::vector<tc::component_registry::Pin> inputPins,outputPins;
        for(uint32_t i=0;i<inputs;++i)inputPins.push_back({std::string(),inputWidths[i]});
        for(uint32_t i=0;i<outputs;++i)outputPins.push_back({std::string(),outputWidths[i]});
        tc::component_registry::noteShape(custom_id,owner_mod?owner_mod:"",inputPins,outputPins);
    }
    return 0;
}
inline int addV2(const V2Registration& registration) {
    return addVersioned(registration,nullptr);
}
inline int addV3(const V3Registration& registration) {
    V2Registration common;
    common.custom_id=registration.custom_id;
    common.user=registration.user;
    common.stateWords=registration.stateWords;
    common.owner_mod=registration.owner_mod;
    common.lifecycle=registration.lifecycle;
    common.configSchema=registration.configSchema;
    common.defaultConfig=registration.defaultConfig;
    common.migrationVersion=registration.migrationVersion;
    common.migrateConfig=registration.migrateConfig;
    common.migrationUser=registration.migrationUser;
    common.outputDependencies=registration.outputDependencies;
    common.clockedTick=registration.clockedTick;
    common.groupedOutputs=registration.groupedOutputs;
    common.inlinePrefixes=registration.inlinePrefixes;
    common.inlineExpressions=registration.inlineExpressions;
    return addVersioned(common,registration.callback);
}
inline void finish(const std::vector<uint64_t>& ids,bool success) {
    std::lock_guard<std::mutex> lock(registryMutex);
    for(auto id:ids) {auto it=definitions.find(id);if(it==definitions.end())continue;
        if(success)it->second->active=true;else definitions.erase(it);}
}
using InputPayload = std::array<uint64_t,kMaxInputWords>;

// The <=128-bit fast path latches the V3 drive plane in one four-argument call.
inline uint64_t latchInputDrive(uint64_t token,uint64_t packedLo,uint64_t packedHi) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active||!binding->definition->useV3)return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        binding->inputDrive.fill(0);
        binding->inputDrive[0]=packedLo;
        binding->inputDrive[1]=packedHi;
        binding->inputDriveReady=true;
        return 1;
    } catch(...) { return 0; }
}

// Runs the callback once and keeps its outputs for the remaining output
// drivers.  `payload` is either the two-word fast path or a complete staged
// batch; each callback pin is reconstructed and masked to its declared width.
inline uint64_t invokePayload(uint64_t token,int64_t cycle,const InputPayload& payload,
                              const InputPayload* stagedDrive,uint32_t phase,
                              uint32_t requestedOutput=0) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active)return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        const uint32_t inputCount=binding->definition->inputs;
        const uint32_t outputCount=binding->definition->outputs;
        if(binding->definition->useV3) {
            InputPayload defaultDrive{};
            const InputPayload* drivePayload=stagedDrive;
            if(!drivePayload) {
                if(binding->inputDriveReady)drivePayload=&binding->inputDrive;
                else {defaultDrive.fill(~uint64_t(0));drivePayload=&defaultDrive;}
            }
            binding->inputDriveReady=false;
            for(uint32_t i=0;i<inputCount;++i) {
                const uint32_t bits=binding->definition->inputWidths[i];
                const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
                const auto word=binding->definition->inputWord[i];
                const auto shift=binding->definition->inputShift[i];
                uint64_t value=payload[word]>>shift;
                uint64_t drive=(*drivePayload)[word]>>shift;
                if(shift+bits>64) {
                    value|=payload[word+1]<<(64-shift);
                    drive|=(*drivePayload)[word+1]<<(64-shift);
                }
                drive&=mask;
                binding->v3Inputs[i]={value&drive,drive};
            }
            std::array<TCLogicValueV3,kMaxBridgeOutputs> callbackOutputs{};
            TCLogicIOV3 io{};
            io.size=sizeof(io);io.version=TC_LOGIC_IO_V3_VERSION_1;io.phase=phase;
            io.instance_id=binding->instance;io.cycle=cycle;
            io.input_count=inputCount;io.output_count=outputCount;
            if(phase==TC_LOGIC_REFRESH) {
                if(binding->refreshState.size()!=binding->state.size())binding->refreshState.resize(binding->state.size());
                std::copy(binding->state.begin(),binding->state.end(),binding->refreshState.begin());
            }
            io.inputs=binding->v3Inputs.data();io.outputs=callbackOutputs.data();
            io.state=binding->state.empty()?nullptr:
                (phase==TC_LOGIC_REFRESH?binding->refreshState.data():binding->state.data());
            io.state_words=static_cast<uint32_t>(binding->state.size());
            /* Private bridge convention: zero means the complete component;
               N+1 asks a dependency-isolated callback to evaluate output N.
               TC-Verilog consumes it, ordinary V3 callbacks may ignore it. */
            io.reserved=requestedOutput;
            io.user=binding->definition->api.user;
            io.config=binding->config.empty()?nullptr:binding->config.data();
            io.config_size=static_cast<uint32_t>(binding->config.size());
            io.config_schema=binding->definition->configSchema;
            binding->definition->callbackV3(&io);
            if(phase==TC_LOGIC_CYCLE)++binding->calls; else ++binding->peeks;
            if(phase==TC_LOGIC_CYCLE&&!binding->state.empty())
                std::copy(io.state,io.state+io.state_words,binding->state.begin());
            for(uint32_t i=0;i<outputCount;++i) {
                bool publish=requestedOutput!=kTickOnlySelector;
                if(publish&&(requestedOutput&kOutputGroupSelector)!=0) {
                    const uint32_t selected=(requestedOutput&~kOutputGroupSelector)-1;
                    publish=selected<binding->definition->outputDependencies.size()&&
                            i<binding->definition->outputDependencies.size()&&
                            binding->definition->outputDependencies[i]==
                                binding->definition->outputDependencies[selected];
                }
                if(!publish)continue;
                const uint32_t bits=binding->definition->outputWidths[i];
                const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
                uint64_t drive=callbackOutputs[i].drive_mask&mask;
                if(drive!=0&&drive!=mask) {
                    drive=0;
                    if(!binding->partialDriveWarned) {
                        binding->partialDriveWarned=true;
                        note("instance "+hex64(binding->instance)+
                             " returned a partial V3 drive mask; the game's word Switch can only drive or disconnect the whole pin");
                    }
                }
                binding->v3Outputs[i].drive_mask=drive;
                binding->v3Outputs[i].value=callbackOutputs[i].value&drive;
                binding->outputs[i]=binding->v3Outputs[i].value;
            }
            if(binding->definition->wideOutputs)
                publishWordOutputs(token,binding->outputs.data(),outputCount);
            return outputCount?binding->outputs[0]:0;
        }
        if(binding->definition->useV2) {
            /* V2: the callback gets the counts and borrowed arrays, so a shape
               with more than eight pins works and the state size is the
               definition's own. */
            for(uint32_t i=0;i<inputCount;++i) {
                const uint32_t bits=binding->definition->inputWidths[i];
                const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
                const auto word=binding->definition->inputWord[i];
                const auto shift=binding->definition->inputShift[i];
                uint64_t value=payload[word]>>shift;
                if(shift+bits>64)value|=payload[word+1]<<(64-shift);
                binding->v2Inputs[i]=value&mask;
            }
            TCLogicIOV2 io{};
            io.size=sizeof(io);io.version=TC_LOGIC_IO_V2_VERSION_1;io.phase=phase;
            io.instance_id=binding->instance;io.cycle=cycle;
            io.input_count=inputCount;io.output_count=outputCount;
            if(phase==TC_LOGIC_REFRESH) {
                if(binding->refreshState.size()!=binding->state.size())binding->refreshState.resize(binding->state.size());
                std::copy(binding->state.begin(),binding->state.end(),binding->refreshState.begin());
            }
            io.inputs=binding->v2Inputs.data();
            io.outputs=binding->outputs.data();
            io.state=binding->state.empty()?nullptr:
                (phase==TC_LOGIC_REFRESH?binding->refreshState.data():binding->state.data());
            io.state_words=static_cast<uint32_t>(binding->state.size());
            io.user=binding->definition->api.user;
            io.config=binding->config.empty()?nullptr:binding->config.data();
            io.config_size=static_cast<uint32_t>(binding->config.size());
            io.config_schema=binding->definition->configSchema;
            binding->definition->callbackV2(&io);
            if(phase==TC_LOGIC_CYCLE)++binding->calls; else ++binding->peeks;
            if(phase==TC_LOGIC_CYCLE&&!binding->state.empty())
                std::copy(io.state,io.state+io.state_words,binding->state.begin());
            if(binding->definition->wideOutputs)
                publishWordOutputs(token,binding->outputs.data(),outputCount);
            return binding->outputs[0];
        }
        TCLogicIO io{};io.size=sizeof(io);io.phase=phase;io.instance_id=binding->instance;io.cycle=cycle;
        io.input_count=inputCount;io.output_count=outputCount;io.user=binding->definition->api.user;
        for(uint32_t i=0;i<inputCount&&i<kMaxBridgeInputs;++i) {
            const uint32_t bits=binding->definition->inputWidths[i];
            const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
            const auto word=binding->definition->inputWord[i],shift=binding->definition->inputShift[i];
            uint64_t value=payload[word]>>shift;
            if(shift+bits>64)value|=payload[word+1]<<(64-shift);
            io.inputs[i]=value&mask;
        }
        const size_t stateWords=std::min<size_t>(8,binding->state.size());
        std::copy(binding->state.begin(),binding->state.begin()+stateWords,io.state);
        binding->definition->api.callback(&io);
        if(phase==TC_LOGIC_CYCLE)++binding->calls; else ++binding->peeks;
        if(phase==TC_LOGIC_CYCLE)
            std::copy(std::begin(io.state),std::begin(io.state)+stateWords,binding->state.begin());
        for(uint32_t i=0;i<8;++i)binding->outputs[i]=io.outputs[i]&1;
        if(binding->definition->wideOutputs)publishWordOutputs(token,io);
        return binding->outputs[0];
    }catch(...) {if(logger)logger("Native logic: callback threw; outputs forced to zero");return 0;}
}

inline uint64_t invoke(uint64_t token,int64_t cycle,uint64_t packedLo,uint64_t packedHi,
                       uint32_t phase) noexcept {
    InputPayload payload{};
    payload[0]=packedLo;payload[1]=packedHi;
    return invokePayload(token,cycle,payload,nullptr,phase);
}

inline uint64_t invokeTick(uint64_t token,int64_t cycle,uint64_t packedLo,
                           uint64_t packedHi) noexcept {
    InputPayload payload{};
    payload[0]=packedLo;payload[1]=packedHi;
    return invokePayload(token,cycle,payload,nullptr,TC_LOGIC_CYCLE,kTickOnlySelector);
}

inline uint64_t invokeOutput(uint64_t token,uint64_t selector,uint64_t packedLo,
                             uint64_t packedHi) noexcept {
    const uint32_t output=static_cast<uint32_t>(selector&31u);
    const int64_t cycle=static_cast<int64_t>(selector>>5)-1;
    InputPayload payload{};
    payload[0]=packedLo;payload[1]=packedHi;
    invokePayload(token,cycle,payload,nullptr,TC_LOGIC_REFRESH,
                  kOutputGroupSelector|(output+1));
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);
         if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active)return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        return output<binding->definition->outputs?binding->outputs[output]&1:0;
    } catch(...) {return 0;}
}

/* Wider inputs keep the JIT-safe four-argument call shape.  The second argument
   encodes `(cycle + 1) * 32 + phase * 16 + chunk`; the +1 bias preserves the
   simulator's initial cycle -1 in this unsigned transport.  Each chunk contributes two words.
   Separate CYCLE/REFRESH banks prevent one phase from consuming the other's
   partially prepared batch. */
inline uint64_t stageInput(uint64_t token,uint64_t selector,uint64_t packedLo,
                           uint64_t packedHi,bool drivePlane) noexcept {
    try {
        const uint32_t chunk=static_cast<uint32_t>(selector&15u);
        const uint32_t stagePhase=static_cast<uint32_t>((selector>>4)&1u);
        const uint64_t cycle=selector>>5;
        if(chunk>=kMaxInputChunks)return 0;
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);
         if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active||(drivePlane&&!binding->definition->useV3))return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        auto& stage=binding->inputStages[stagePhase];
        auto& words=drivePlane?stage.drives:stage.values;
        auto& stagedCycle=drivePlane?stage.driveCycle:stage.valueCycle;
        auto& cycleValid=drivePlane?stage.driveCycleValid:stage.valueCycleValid;
        auto& chunks=drivePlane?stage.driveChunks:stage.valueChunks;
        if(!cycleValid||stagedCycle!=cycle) {
            words.fill(0);chunks=0;stagedCycle=cycle;cycleValid=true;
        }
        words[chunk*kInputWordsPerCall]=packedLo;
        words[chunk*kInputWordsPerCall+1]=packedHi;
        chunks|=uint32_t(1)<<chunk;
        return 1;
    } catch(...) { return 0; }
}

inline uint64_t invokeStaged(uint64_t token,int64_t cycle,uint64_t chunkCount,
                             uint32_t phase,uint32_t requestedOutput=0) noexcept {
    try {
        if(chunkCount<=1||chunkCount>kMaxInputChunks)return 0;
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);
         if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active)return 0;
        InputPayload values{},drives{};
        bool useV3=false,ready=false;
        {
            std::lock_guard<std::mutex> lock(binding->mutex);
            const uint32_t stagePhase=phase==TC_LOGIC_REFRESH?1u:0u;
            auto& stage=binding->inputStages[stagePhase];
            const uint32_t expected=(uint32_t(1)<<static_cast<uint32_t>(chunkCount))-1u;
            const uint64_t expectedCycle=static_cast<uint64_t>(cycle)+1;
            useV3=binding->definition->useV3;
            ready=stage.valueCycleValid&&stage.valueCycle==expectedCycle&&
                  (stage.valueChunks&expected)==expected;
            if(useV3)
                ready=ready&&stage.driveCycleValid&&stage.driveCycle==expectedCycle&&
                      (stage.driveChunks&expected)==expected;
            if(ready) {values=stage.values;if(useV3)drives=stage.drives;}
            stage.valueChunks=stage.driveChunks=0;
            stage.valueCycleValid=stage.driveCycleValid=false;
            if(!ready&&!binding->inputStageWarned) {
                binding->inputStageWarned=true;
                note("instance token "+std::to_string(token)+
                     " received an incomplete staged input batch");
            }
        }
        if(!ready)return 0;
        return invokePayload(token,cycle,values,useV3?&drives:nullptr,phase,requestedOutput);
    } catch(...) { return 0; }
}
/* Independent output evaluation consumes the same staged batch but always
   executes the callback as REFRESH.  The game's CYCLE/REFRESH distinction only
   selects the staging bank; it must not advance callback state once per output. */
inline uint64_t invokeStagedOutput(uint64_t token,int64_t cycle,uint64_t chunkCount,
                                   uint32_t stagePhase,uint32_t output) noexcept {
    try {
        if(chunkCount<=1||chunkCount>kMaxInputChunks)return 0;
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);
         if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active)return 0;
        InputPayload values{},drives{};
        bool useV3=false,ready=false;
        {
            std::lock_guard<std::mutex> lock(binding->mutex);
            auto& stage=binding->inputStages[stagePhase==TC_LOGIC_REFRESH?1u:0u];
            const uint32_t expected=(uint32_t(1)<<static_cast<uint32_t>(chunkCount))-1u;
            const uint64_t expectedCycle=static_cast<uint64_t>(cycle)+1;
            useV3=binding->definition->useV3;
            ready=stage.valueCycleValid&&stage.valueCycle==expectedCycle&&
                  (stage.valueChunks&expected)==expected;
            if(useV3)
                ready=ready&&stage.driveCycleValid&&stage.driveCycle==expectedCycle&&
                      (stage.driveChunks&expected)==expected;
            if(ready){values=stage.values;if(useV3)drives=stage.drives;}
            stage.valueChunks=stage.driveChunks=0;
            stage.valueCycleValid=stage.driveCycleValid=false;
        }
        if(!ready)return 0;
        invokePayload(token,cycle,values,useV3?&drives:nullptr,TC_LOGIC_REFRESH,
                      kOutputGroupSelector|(output+1));
        std::lock_guard<std::mutex> lock(binding->mutex);
        return output<binding->definition->outputs?binding->outputs[output]&1:0;
    } catch(...) {return 0;}
}
inline uint64_t readOutput(uint64_t token,uint64_t index) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active)return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        return index<kMaxBridgeOutputs?binding->outputs[index]&1:0;
    }catch(...) {return 0;}
}
inline uint64_t readOutputDrive(uint64_t token,uint64_t index) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        if(!binding||!binding->active||!binding->definition->useV3)return 0;
        std::lock_guard<std::mutex> lock(binding->mutex);
        if(index>=binding->definition->outputs||index>=kMaxBridgeOutputs)return 0;
        const uint32_t bits=binding->definition->outputWidths[static_cast<size_t>(index)];
        const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
        return binding->v3Outputs[static_cast<size_t>(index)].drive_mask==mask?1:0;
    }catch(...) {return 0;}
}
// One bit of a previously computed output word.  Word-width pins are assembled
// bit by bit in the generated program because the JIT only consumes foreign
// call results reliably as single bits.
inline uint64_t readOutputBit(uint64_t token,uint64_t index) noexcept {
    try {
        const uint32_t pin=static_cast<uint32_t>(index>>6);
        const uint32_t bit=static_cast<uint32_t>(index&63);
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        std::lock_guard<std::mutex> lock(binding->mutex);
        if(pin>=kMaxBridgeOutputs)return 0;
        return (binding->outputs[pin]>>bit)&1;
    }catch(...) {return 0;}
}
inline void reset() noexcept {
    try {
        std::vector<std::shared_ptr<Binding>> snapshot;
        {std::lock_guard<std::mutex> lock(registryMutex);snapshot=bindings;}
        for(auto& binding:snapshot){std::lock_guard<std::mutex> lock(binding->mutex);
            if(!binding||!binding->active)continue;
            ++binding->resets;
            std::fill(binding->state.begin(),binding->state.end(),0);
            binding->outputs.fill(0);
            std::fill(binding->v3Inputs.begin(),binding->v3Inputs.end(),TCLogicValueV3{});
            std::fill(binding->v3Outputs.begin(),binding->v3Outputs.end(),TCLogicValueV3{});
            binding->inputDrive.fill(0);
            binding->inputDriveReady=false;
            binding->inputStages={};
            binding->inputStageWarned=false;
            if(binding->definition->useV3) {
                TCLogicIOV3 io{};io.size=sizeof(io);io.version=TC_LOGIC_IO_V3_VERSION_1;
                io.phase=TC_LOGIC_RESET;io.instance_id=binding->instance;io.cycle=-1;
                io.input_count=binding->definition->inputs;
                io.output_count=binding->definition->outputs;
                io.inputs=binding->v3Inputs.data();io.outputs=binding->v3Outputs.data();
                io.state=binding->state.empty()?nullptr:binding->state.data();
                io.state_words=static_cast<uint32_t>(binding->state.size());
                io.user=binding->definition->api.user;
                io.config=binding->config.empty()?nullptr:binding->config.data();
                io.config_size=static_cast<uint32_t>(binding->config.size());
                io.config_schema=binding->definition->configSchema;
                binding->definition->callbackV3(&io);
                if(!binding->state.empty())
                    std::copy(io.state,io.state+io.state_words,binding->state.begin());
                continue;
            }
            if(binding->definition->useV2) {
                TCLogicIOV2 io{};io.size=sizeof(io);io.version=TC_LOGIC_IO_V2_VERSION_1;
                io.phase=TC_LOGIC_RESET;io.instance_id=binding->instance;io.cycle=-1;
                io.input_count=binding->definition->inputs;
                io.output_count=binding->definition->outputs;
                io.inputs=binding->v2Inputs.data();io.outputs=binding->outputs.data();
                io.state=binding->state.empty()?nullptr:binding->state.data();
                io.state_words=static_cast<uint32_t>(binding->state.size());
                io.user=binding->definition->api.user;
                io.config=binding->config.empty()?nullptr:binding->config.data();
                io.config_size=static_cast<uint32_t>(binding->config.size());
                io.config_schema=binding->definition->configSchema;
                binding->definition->callbackV2(&io);
                if(!binding->state.empty())
                    std::copy(io.state,io.state+io.state_words,binding->state.begin());
                continue;
            }
            TCLogicIO io{};io.size=sizeof(io);io.phase=TC_LOGIC_RESET;
            io.instance_id=binding->instance;io.cycle=-1;io.user=binding->definition->api.user;
            io.input_count=binding->definition->inputs;io.output_count=binding->definition->outputs;
            binding->definition->api.callback(&io);
            std::copy(std::begin(io.state),
                      std::begin(io.state)+std::min<size_t>(8,binding->state.size()),
                      binding->state.begin());}
        if(!snapshot.empty())note("reset "+std::to_string(snapshot.size())+" instance(s)");
    }catch(...) {if(logger)logger("Native logic: reset callback threw");}
}
/* ---- instances: lifecycle, handles and queries -----------------------------

   A binding is the host's record of one live component instance: its type, the
    game's instance id, a host-issued generation, and the simulation state the
   callback owns.  Tokens (used by the generated program) index this vector, so
   a released instance keeps its slot but is marked inactive - releasing in the
   middle must not move the tokens of the others.  Reusing a slot issues a fresh
   generation, which is what makes an old handle stale instead of resolving to
   whatever lives there now. */
inline void callV2Locked(Binding& binding,uint32_t phase,TCLogicCallbackV2 callback,
                         int64_t cycle) {
    auto* definition=binding.definition.get();
    TCLogicIOV2 io{};
    io.size=sizeof(io);io.version=TC_LOGIC_IO_V2_VERSION_1;io.phase=phase;
    io.instance_id=binding.instance;io.cycle=cycle;
    io.input_count=definition->inputs;io.output_count=definition->outputs;
    io.inputs=binding.v2Inputs.data();io.outputs=binding.outputs.data();
    io.state=binding.state.empty()?nullptr:binding.state.data();
    io.state_words=static_cast<uint32_t>(binding.state.size());
    io.user=definition->api.user;
    io.config=binding.config.empty()?nullptr:binding.config.data();
    io.config_size=static_cast<uint32_t>(binding.config.size());
    io.config_schema=definition->configSchema;
    /* The placement tail (see TCLogicIOV2): every lifecycle phase may read it,
       and the phases that change it are MOVE / ROTATE / RESIZE. */
    io.cell_x=binding.cellX;
    io.cell_y=binding.cellY;
    io.rotation=binding.rotation;
    io.half_width=binding.halfWidth;
    io.half_height=binding.halfHeight;
    callback(&io);
    /* Lifecycle and RESET are not simulation phases: their writes are committed
       immediately, because there is no cycle for them to be discarded with. */
    if(!binding.state.empty())std::copy(io.state,io.state+io.state_words,binding.state.begin());
}
/* Caller holds the binding's mutex. */
inline void notifyCreate(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(lifecycle&&lifecycle->on_create) {
        try {callV2Locked(binding,TC_LOGIC_CREATE,lifecycle->on_create,-1);}
        catch(...) {note("lifecycle on_create threw for instance "+hex64(binding.instance));}
    }
    binding.created=true;
}
/* Caller holds the binding's mutex. */
inline void notifyDestroy(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle||!lifecycle->on_destroy)return;
    try {callV2Locked(binding,TC_LOGIC_DESTROY,lifecycle->on_destroy,-1);}
    catch(...) {note("lifecycle on_destroy threw for instance "+hex64(binding.instance));}
}
/* A configuration change is announced with the binding's mutex *released*: a
   plugin may call the storage services from this callback, and those take the
   same lock.  The callback reads a copy of the configuration and of the state,
   and whatever it writes into the state copy is committed afterwards. */
inline void notifyConfigChanged(const std::shared_ptr<Binding>& binding,
                                const std::vector<uint8_t>& config) {
    const TCComponentLifecycleV1* lifecycle=binding->definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_config_changed)+
                      sizeof(lifecycle->on_config_changed);
    if(lifecycle->size<tail||!lifecycle->on_config_changed)return;
    const auto* definition=binding->definition.get();
    std::vector<uint64_t> state;
    {
        std::lock_guard<std::mutex> lock(binding->mutex);
        state=binding->state;
    }
    TCLogicIOV2 io{};
    io.size=sizeof(io);io.version=TC_LOGIC_IO_V2_VERSION_1;io.phase=TC_LOGIC_CONFIG_CHANGED;
    io.instance_id=binding->instance;io.cycle=-1;
    io.input_count=definition->inputs;io.output_count=definition->outputs;
    io.inputs=binding->v2Inputs.data();io.outputs=binding->outputs.data();
    io.state=state.empty()?nullptr:state.data();
    io.state_words=static_cast<uint32_t>(state.size());
    io.user=definition->api.user;
    io.config=config.empty()?nullptr:const_cast<uint8_t*>(config.data());
    io.config_size=static_cast<uint32_t>(config.size());
    io.config_schema=definition->configSchema;
    try {
        lifecycle->on_config_changed(&io);
    } catch(...) {
        note("lifecycle on_config_changed threw for instance "+hex64(binding->instance));
        return;
    }
    if(!state.empty()) {
        std::lock_guard<std::mutex> lock(binding->mutex);
        std::copy(io.state,io.state+io.state_words,binding->state.begin());
    }
}
/* Same notification, addressed the way an undo step knows its instance.  The
   lookup is the same one every storage call uses, so a step whose record left the
   board simply has nobody to tell. */
inline void notifyConfigChangedByKey(uint64_t customId,uint64_t instance,
                                     const std::vector<uint8_t>& config) {
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(customId,instance));
        if(found==bindingKeys.end())return;
        const size_t slot=static_cast<size_t>(found->second);
        if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return;
        binding=bindings[slot-1];
    }
    notifyConfigChanged(binding,config);
}
/* Fired for an instance the host created by duplicating another one.  It runs
   after on_create, with the copied configuration already installed, so a plugin
   can hand out a fresh identity or drop state that must not be shared. */
inline void notifyClone(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_clone)+sizeof(lifecycle->on_clone);
    if(lifecycle->size<tail||!lifecycle->on_clone)return;
    try {
        callV2Locked(binding,TC_LOGIC_CLONE,lifecycle->on_clone,-1);
    } catch(...) {
        note("lifecycle on_clone threw for instance "+hex64(binding.instance));
    }
}
/* The configuration came out of the circuit file.  Fired after on_create, and
   instead of on_clone for a copy (whose bytes the host wrote itself). */
inline void notifyLoad(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_load)+sizeof(lifecycle->on_load);
    if(lifecycle->size<tail||!lifecycle->on_load)return;
    try {
        callV2Locked(binding,TC_LOGIC_LOAD,lifecycle->on_load,-1);
    } catch(...) {
        note("lifecycle on_load threw for instance "+hex64(binding.instance));
    }
}
/* ---- placement: MOVE / ROTATE / RESIZE -------------------------------------

   The board record is the authority for where an instance sits and which way it
   points, so the host reads it once per frame (native.hpp's poll) and hands the
   reading here; the binding keeps the last reading and only a *change* becomes a
   callback.  The first reading seeds the binding - being compiled is not a
   move.  All three callbacks run with the binding's lock held, like
   on_create/on_destroy, and read the new values from the IO's placement tail. */
inline void notifyMove(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_move)+sizeof(lifecycle->on_move);
    if(lifecycle->size<tail||!lifecycle->on_move)return;
    try {
        callV2Locked(binding,TC_LOGIC_MOVE,lifecycle->on_move,-1);
    } catch(...) {
        note("lifecycle on_move threw for instance "+hex64(binding.instance));
    }
}
inline void notifyRotate(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_rotate)+sizeof(lifecycle->on_rotate);
    if(lifecycle->size<tail||!lifecycle->on_rotate)return;
    try {
        callV2Locked(binding,TC_LOGIC_ROTATE,lifecycle->on_rotate,-1);
    } catch(...) {
        note("lifecycle on_rotate threw for instance "+hex64(binding.instance));
    }
}
inline void notifyResize(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_resize)+sizeof(lifecycle->on_resize);
    if(lifecycle->size<tail||!lifecycle->on_resize)return;
    try {
        callV2Locked(binding,TC_LOGIC_RESIZE,lifecycle->on_resize,-1);
    } catch(...) {
        note("lifecycle on_resize threw for instance "+hex64(binding.instance));
    }
}
/* The board compiled with this instance in it: once per compile, after the
   bindings for that compile exist. */
inline void notifyCompile(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_compile)+sizeof(lifecycle->on_compile);
    if(lifecycle->size<tail||!lifecycle->on_compile)return;
    try {
        callV2Locked(binding,TC_LOGIC_COMPILE,lifecycle->on_compile,-1);
    } catch(...) {
        note("lifecycle on_compile threw for instance "+hex64(binding.instance));
    }
}
/* The host dropped the binding while the instance itself was not deleted: the
   board went away (leaving the level) or the loader is shutting down.  A
   component the player *deleted* arrives as TC_LOGIC_DESTROY instead. */
inline void notifyUnbind(Binding& binding) {
    const TCComponentLifecycleV1* lifecycle=binding.definition->lifecycle;
    if(!lifecycle)return;
    const size_t tail=offsetof(TCComponentLifecycleV1,on_unbind)+sizeof(lifecycle->on_unbind);
    if(lifecycle->size<tail||!lifecycle->on_unbind)return;
    try {
        callV2Locked(binding,TC_LOGIC_UNBIND,lifecycle->on_unbind,-1);
    } catch(...) {
        note("lifecycle on_unbind threw for instance "+hex64(binding.instance));
    }
}
/* The host's per-frame reading of one instance's board record (native.hpp's
   poll).  `rotation` is the record's direction byte; the first reading seeds the
   binding and fires nothing. */
inline void updateInstanceGeometry(uint64_t customId,uint64_t instance,
                                   int32_t cellX,int32_t cellY,uint32_t rotation) {
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(customId,instance));
        if(found==bindingKeys.end())return;
        const size_t slot=static_cast<size_t>(found->second);
        if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return;
        binding=bindings[slot-1];
    }
    std::lock_guard<std::mutex> lock(binding->mutex);
    if(!binding->geometryKnown) {
        binding->cellX=cellX;binding->cellY=cellY;binding->rotation=rotation;
        binding->geometryKnown=true;
        return;
    }
    const bool moved=binding->cellX!=cellX||binding->cellY!=cellY;
    const bool rotated=binding->rotation!=rotation;
    binding->cellX=cellX;binding->cellY=cellY;binding->rotation=rotation;
    if(moved)notifyMove(*binding);
    if(rotated)notifyRotate(*binding);
}
/* The footprint the host announced for one instance (tc.component.geometry's
   per-instance rectangle, or the type's own declaration once the instance
   exists).  A change fires TC_LOGIC_RESIZE; the first one only seeds. */
inline void updateInstanceFootprint(uint64_t customId,uint64_t instance,
                                    float halfWidth,float halfHeight) {
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(customId,instance));
        if(found==bindingKeys.end())return;
        const size_t slot=static_cast<size_t>(found->second);
        if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return;
        binding=bindings[slot-1];
    }
    std::lock_guard<std::mutex> lock(binding->mutex);
    if(binding->sizeKnown&&binding->halfWidth==halfWidth&&binding->halfHeight==halfHeight)return;
    binding->halfWidth=halfWidth;binding->halfHeight=halfHeight;
    binding->sizeKnown=true;
    /* Any change is worth announcing, including the first: the instance's size
       genuinely stopped being the type's declared default. */
    notifyResize(*binding);
}
/* Every live instance of the current board hears that the game is about to write
   a circuit out.  The callbacks run with their own binding lock held (like
   on_create) but the registry lock is released first, so a callback may submit
   commands or write its configuration. */
inline void notifySave() {
    std::vector<std::shared_ptr<Binding>> live;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        for(auto& binding:bindings)
            if(binding&&binding->active)live.push_back(binding);
    }
    for(auto& binding:live) {
        const TCComponentLifecycleV1* lifecycle=binding->definition->lifecycle;
        if(!lifecycle)continue;
        const size_t tail=offsetof(TCComponentLifecycleV1,on_save)+sizeof(lifecycle->on_save);
        if(lifecycle->size<tail||!lifecycle->on_save)continue;
        /* The callback runs with the binding lock released (like on_config_changed):
           the whole point of this moment is that a definition may commit a
           configuration from it, and write_config takes that same lock. */
        std::vector<uint8_t> config;
        std::vector<uint64_t> state;
        {
            std::lock_guard<std::mutex> lock(binding->mutex);
            config=binding->config;
            state=binding->state;
        }
        const auto* definition=binding->definition.get();
        TCLogicIOV2 io{};
        io.size=sizeof(io);io.version=TC_LOGIC_IO_V2_VERSION_1;io.phase=TC_LOGIC_SAVE;
        io.instance_id=binding->instance;io.cycle=-1;
        io.input_count=definition->inputs;io.output_count=definition->outputs;
        io.inputs=binding->v2Inputs.data();io.outputs=binding->outputs.data();
        io.state=state.empty()?nullptr:state.data();
        io.state_words=static_cast<uint32_t>(state.size());
        io.user=definition->api.user;
        io.config=config.empty()?nullptr:const_cast<uint8_t*>(config.data());
        io.config_size=static_cast<uint32_t>(config.size());
        io.config_schema=definition->configSchema;
        try {
            lifecycle->on_save(&io);
        } catch(...) {
            note("lifecycle on_save threw for instance "+hex64(binding->instance));
            continue;
        }
        if(!state.empty()) {
            std::lock_guard<std::mutex> lock(binding->mutex);
            std::copy(io.state,io.state+io.state_words,binding->state.begin());
        }
    }
}
/* Releases the instances a compile no longer contains; `survivors` null releases
   everything (the board went away).  Only called for a compile that really is
   the board: the level's own test program compiles too, and it contains none of
   our instances - the caller checks that before calling this. */
/* Kept byte images of configuration edits are dropped with the instance that
   owned them; defined with the storage section below. */
inline void dropConfigEdits(uint64_t customId,uint64_t instance);
inline void releaseInstances(const std::set<std::pair<uint64_t,uint64_t>>* survivors,
                             const char* reason) {
    std::vector<std::shared_ptr<Binding>> dropped;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        for(auto& binding:bindings) {
            if(!binding||!binding->active)continue;
            const auto key=std::make_pair(binding->definition->api.custom_id,binding->instance);
            if(survivors&&survivors->count(key))continue;
            binding->active=false;
            bindingKeys.erase(key);
            dropped.push_back(binding);
            /* A released instance's kept byte images can no longer be applied:
               the id may name a different component after the next bind. */
            dropConfigEdits(binding->definition->api.custom_id,binding->instance);
            binding->editOpen=false;
            binding->editBefore.clear();
        }
    }
    for(auto& binding:dropped) {
        std::lock_guard<std::mutex> lock(binding->mutex);
        /* Two different events share this path: a compile that no longer contains
           the instance means the player deleted it (TC_LOGIC_DESTROY), while
           "everything" (survivors == null) means the board itself went away -
           leaving the level, or the loader shutting down - and the component is
           still out there in the schematic (TC_LOGIC_UNBIND). */
        if(survivors)notifyDestroy(*binding);
        else notifyUnbind(*binding);
    }
    if(!dropped.empty())
        note("released "+std::to_string(dropped.size())+" instance(s) ("+reason+")");
}
inline size_t liveInstanceCount() {
    std::lock_guard<std::mutex> lock(registryMutex);
    size_t live=0;
    for(auto& binding:bindings)if(binding&&binding->active)++live;
    return live;
}
/* Resolves a handle to the live binding it names, honouring the generation. */
inline bool instanceMatches(const TCComponentInstanceHandle* handle,
                            std::shared_ptr<Binding>& out) {
    if(!handle||handle->size<sizeof(*handle)||
       handle->version!=TC_COMPONENT_INSTANCES_API_VERSION_1)return false;
    std::lock_guard<std::mutex> lock(registryMutex);
    for(auto& binding:bindings) {
        if(!binding||!binding->active)continue;
        if(binding->definition->api.custom_id!=handle->custom_id)continue;
        if(binding->instance!=handle->instance_id)continue;
        if(binding->generation!=handle->generation)return false;   /* reused slot */
        out=binding;
        return true;
    }
    return false;   /* gone, or never existed: the caller sees ERR_STALE */
}
inline int instanceValidate(const TCComponentInstanceHandle* handle) {
    std::shared_ptr<Binding> binding;
    return instanceMatches(handle,binding)?TC_COMPONENT_INSTANCES_OK
                                         :TC_COMPONENT_INSTANCES_ERR_STALE;
}
/* tc.component.instances V2: the id the game addresses this component by when
   it is selected or walked from the top.  Captured by the compile walk (see
   Binding::topLevel), so this is a lookup, not a re-read of the board - an
   instance handle only exists for a component that walk bound. */
inline int instanceTopLevelId(const TCComponentInstanceHandle* handle,uint64_t* out) {
    if(!handle||!out)return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    *out=0;
    std::lock_guard<std::mutex> lock(registryMutex);
    for(auto& binding:bindings) {
        if(!binding||!binding->active)continue;
        if(binding->definition->api.custom_id!=handle->custom_id)continue;
        if(binding->instance!=handle->instance_id)continue;
        if(binding->generation!=handle->generation)return TC_COMPONENT_INSTANCES_ERR_STALE;
        *out=binding->topLevel?binding->topLevel:binding->instance;
        return TC_COMPONENT_INSTANCES_OK;
    }
    return TC_COMPONENT_INSTANCES_ERR_STALE;
}
/* Builds the handle for an instance the host is already naming by its
   (custom id, board id) pair - the way the component drawer and the editor rows
   identify what they are showing.  False when no live binding carries that
   pair, so the caller keeps its "not bound yet" path instead of inventing a
   generation. */
inline bool instanceHandleOf(uint64_t customId,uint64_t instance,
                             TCComponentInstanceHandle* out) {
    if(!out)return false;
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto found=bindingKeys.find(std::make_pair(customId,instance));
    if(found==bindingKeys.end())return false;
    const size_t slot=static_cast<size_t>(found->second);
    if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return false;
    out->size=sizeof(*out);
    out->version=TC_COMPONENT_INSTANCES_API_VERSION_1;
    out->custom_id=customId;
    out->instance_id=instance;
    out->generation=bindings[slot-1]->generation;
    return true;
}
inline int instanceEnumerate(uint64_t custom_id,TCComponentInstanceHandle* out,uint32_t capacity,
                             uint32_t* written,uint32_t* total) {
    if(!out||capacity==0)return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    uint32_t count=0,all=0;
    std::lock_guard<std::mutex> lock(registryMutex);
    for(auto& binding:bindings) {
        if(!binding||!binding->active)continue;
        if(custom_id&&binding->definition->api.custom_id!=custom_id)continue;
        ++all;
        if(count>=capacity)continue;
        TCComponentInstanceHandle& handle=out[count++];
        handle={};
        handle.size=sizeof(handle);
        handle.version=TC_COMPONENT_INSTANCES_API_VERSION_1;
        handle.custom_id=binding->definition->api.custom_id;
        handle.instance_id=binding->instance;
        handle.generation=binding->generation;
    }
    if(written)*written=count;
    if(total)*total=all;
    return count==all?TC_COMPONENT_INSTANCES_OK:TC_COMPONENT_INSTANCES_ERR_RANGE;
}
inline int instanceInfo(const TCComponentInstanceHandle* handle,TCComponentInstanceInfoV1* out) {
    if(!out||out->size<sizeof(*out))return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_INSTANCES_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    TCComponentInstanceInfoV1 info{};
    info.size=sizeof(info);
    info.handle=*handle;
    info.flags=TC_COMPONENT_INSTANCE_BOUND|(binding->state.empty()?0u:TC_COMPONENT_INSTANCE_HAS_STATE);
    info.state_words=static_cast<uint32_t>(binding->state.size());
    info.input_count=binding->definition->inputs;
    info.output_count=binding->definition->outputs;
    info.cycle_calls=binding->calls;
    info.peeks=binding->peeks;
    info.resets=binding->resets;
    tc::component_registry::Type type;
    if(tc::component_registry::find(handle->custom_id,&type)) {
        std::snprintf(info.type_id,sizeof(info.type_id),"%s/0x%llx",
                      type.owner_mod.c_str(),static_cast<unsigned long long>(handle->custom_id));
        std::snprintf(info.owner_mod,sizeof(info.owner_mod),"%s",type.owner_mod.c_str());
    } else {
        std::snprintf(info.type_id,sizeof(info.type_id),"0x%llx",
                      static_cast<unsigned long long>(handle->custom_id));
    }
    *out=info;
    return TC_COMPONENT_INSTANCES_OK;
}
inline int instanceState(const TCComponentInstanceHandle* handle,uint64_t* out,uint32_t capacity,
                         uint32_t* words) {
    if(!out||capacity==0)return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_INSTANCES_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    const uint32_t count=static_cast<uint32_t>(binding->state.size());
    if(words)*words=count;
    if(count>capacity)return TC_COMPONENT_INSTANCES_ERR_RANGE;
    std::copy(binding->state.begin(),binding->state.end(),out);
    return TC_COMPONENT_INSTANCES_OK;
}
inline int instanceReset(const TCComponentInstanceHandle* handle) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_INSTANCES_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    std::fill(binding->state.begin(),binding->state.end(),0);
    binding->outputs.fill(0);
    ++binding->resets;
    auto* definition=binding->definition.get();
    try {
        if(definition->useV2&&definition->callbackV2) {
            callV2Locked(*binding,TC_LOGIC_RESET,definition->callbackV2,-1);
        } else if(definition->api.callback) {
            TCLogicIO io{};
            io.size=sizeof(io);io.phase=TC_LOGIC_RESET;
            io.instance_id=binding->instance;io.cycle=-1;
            io.user=definition->api.user;
            io.input_count=definition->inputs;io.output_count=definition->outputs;
            const size_t stateWords=std::min<size_t>(8,binding->state.size());
            definition->api.callback(&io);
            std::copy(std::begin(io.state),std::begin(io.state)+stateWords,
                      binding->state.begin());
        }
    } catch(...) {note("reset callback threw for instance "+hex64(binding->instance));}
    return TC_COMPONENT_INSTANCES_OK;
}

/* ---- storage: configuration vs simulation state ---------------------------

   Configuration and simulation state share the binding's lifetime and mutex,
   but not reset semantics.  The public service only performs whole-blob
   replacements so a callback never observes a half-written configuration. */
/* ---- the durable half of a configuration ----------------------------------

   The runtime owns the game-side half of persistence: finding the component
   record that belongs to (custom_id, instance_id) in the *current* Board, and
   reading or writing its key/value table through the game's own functions.  The
   logic layer owns the record format (src/component_tail.hpp).  Neither half
   stores a record or table pointer anywhere: a board edit replaces both, so
   every call resolves the record again.

   With no accessor installed - an unsupported build profile, or a game thread
   this runtime did not start on - configuration stays in memory and `info`
   reports by clearing TC_COMPONENT_STORAGE_HAS_PERSISTENCE. */
struct TailAccess {
    void* context=nullptr;
    /* The live table of (custom_id, instance), or false when the record is not
       on the current Board. */
    bool (*find)(void* context,uint64_t custom_id,uint64_t instance,void** table)=nullptr;
    /* Reads one key.  false = the table could not be walked at all. */
    bool (*read)(void* context,void* table,uint64_t key,uint64_t* value,bool* found)=nullptr;
    /* Writes one key through the game's own setter.  false = the write failed. */
    bool (*write)(void* context,void* table,uint64_t key,uint64_t value)=nullptr;
};
inline TailAccess tailAccess;
/* ---- configuration edit transactions ---------------------------------------

   The game's own undo stack has no record for a configuration change: its entry
   kinds describe board edits (place/rotate/delete/…), and the commands that write
   a value do not register one at all.  So the host keeps the configuration's own
   undo/redo stack: one successful write_config is one step, holding the bytes
   before and after.  The runtime intercepts the game's undo/redo entry and, while
   such a step is pending, replays it into the component record instead of the
   game's own stack - which is what makes one Ctrl+Z revert exactly one
   configuration commit. */
struct ConfigEdit {
    uint64_t customId=0,instance=0;
    uint32_t schema=0;                 /* the schema the bytes belong to */
    std::vector<uint8_t> before,after;
};
inline std::vector<ConfigEdit> configUndoStack,configRedoStack;
inline bool tailAccessAvailable();
/* Defined with the rest of the edit stack below; releasing an instance drops its
   kept byte images. */
inline void dropConfigEdits(uint64_t customId,uint64_t instance);
/* Applies one of a step's two sides to the live record.  Returns false when the
   record is not on the current Board (the instance is gone, so there is nothing
   to undo). */
/* Defined below with the lifecycle callbacks; an undo or redo is a configuration
   change like any other. */
inline void notifyConfigChangedByKey(uint64_t customId,uint64_t instance,
                                     const std::vector<uint8_t>& config);
inline bool applyConfigEdit(const ConfigEdit& edit,const std::vector<uint8_t>& bytes){
    if(!tailAccessAvailable()||!tailAccess.write)return false;
    void* table=nullptr;
    if(!tailAccess.find(tailAccess.context,edit.customId,edit.instance,&table)||!table)return false;
    const auto entries=component_tail::encode(edit.customId,edit.schema,bytes.data(),
                                              static_cast<uint32_t>(bytes.size()));
    for(const auto& entry:entries)
        if(!tailAccess.write(tailAccess.context,table,entry.key,entry.value))return false;
    /* The record is what the circuit carries, but the host's own view is what the
       storage service hands out next: a reverted step has to move both, or a
       plugin would read the superseded bytes. */
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(edit.customId,edit.instance));
        if(found!=bindingKeys.end()){
            const size_t slot=static_cast<size_t>(found->second);
            if(slot>0&&slot<=bindings.size()&&bindings[slot-1]&&bindings[slot-1]->active)
                binding=bindings[slot-1];
        }
    }
    if(binding){
        std::lock_guard<std::mutex> lock(binding->mutex);
        if(binding->config.size()==bytes.size()){
            binding->config=bytes;
            ++binding->configRevision;
            if(!binding->configRevision)binding->configRevision=1;
        }
    }
    return true;
}
inline bool undoConfigEdit(){
    if(configUndoStack.empty())return false;
    const ConfigEdit edit=configUndoStack.back();
    configUndoStack.pop_back();
    if(!applyConfigEdit(edit,edit.before))return false;
    configRedoStack.push_back(edit);
    /* The instance's configuration really changed: the same notification the
       storage service sends for a write goes out for a reverted step. */
    notifyConfigChangedByKey(edit.customId,edit.instance,edit.before);
    return true;
}
inline bool redoConfigEdit(){
    if(configRedoStack.empty())return false;
    const ConfigEdit edit=configRedoStack.back();
    configRedoStack.pop_back();
    if(!applyConfigEdit(edit,edit.after))return false;
    configUndoStack.push_back(edit);
    notifyConfigChangedByKey(edit.customId,edit.instance,edit.after);
    return true;
}
inline void dropConfigEdits(uint64_t customId,uint64_t instance){
    const auto strip=[&](std::vector<ConfigEdit>& stack){
        std::vector<ConfigEdit> kept;
        for(auto& edit:stack)
            if(edit.customId!=customId||edit.instance!=instance)kept.push_back(edit);
        stack.swap(kept);
    };
    strip(configUndoStack);strip(configRedoStack);
}
/* A board change makes the kept byte images meaningless: the records they belong
   to are gone, and a later id could name a different component. */
inline void clearConfigEdits(){configUndoStack.clear();configRedoStack.clear();}
/* Instances the host created by duplicating another one: the record copy is what
   carries the configuration, this only remembers that the next bind has to
   announce it. */
inline std::map<std::pair<uint64_t,uint64_t>,std::vector<uint8_t>> pendingClones;
/* The host knows exactly which bytes it copied, so it hands them to the bind
   instead of hoping the record is reachable at that moment (a compile works on a
   flattened sequence, and the Board's own array may not be readable yet). */
inline void markClone(uint64_t customId,uint64_t instance,std::vector<uint8_t> config){
    std::lock_guard<std::mutex> lock(registryMutex);
    pendingClones[std::make_pair(customId,instance)]=std::move(config);
}
/* The configuration the host holds for an instance, if it is bound: the duplication
   path prefers this over re-decoding the record, so a copy is deterministic even
   when the record is mid-flight (the service may be the only writer that knows). */
inline std::vector<uint8_t> configOfInstance(uint64_t customId,uint64_t instance){
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(customId,instance));
        if(found==bindingKeys.end())return {};
        const size_t slot=static_cast<size_t>(found->second);
        if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return {};
        binding=bindings[slot-1];
    }
    std::lock_guard<std::mutex> lock(binding->mutex);
    return binding->config;
}
inline uint32_t configSchemaOfInstance(uint64_t customId,uint64_t instance){
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found=bindingKeys.find(std::make_pair(customId,instance));
        if(found==bindingKeys.end())return 0;
        const size_t slot=static_cast<size_t>(found->second);
        if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)return 0;
        binding=bindings[slot-1];
    }
    std::lock_guard<std::mutex> lock(binding->mutex);
    return binding->definition?binding->definition->configSchema:0;
}
inline void clearPendingClones(){
    std::lock_guard<std::mutex> lock(registryMutex);
    pendingClones.clear();
}
/* The stack is a convenience, not a history: it is bounded so a long editing
   session cannot grow the host without limit. */
inline constexpr size_t kMaxConfigEditSteps=64;
inline void trimConfigEdits(){
    while(configUndoStack.size()>kMaxConfigEditSteps)configUndoStack.erase(configUndoStack.begin());
    while(configRedoStack.size()>kMaxConfigEditSteps)configRedoStack.erase(configRedoStack.begin());
}
inline bool tailAccessAvailable(){return tailAccess.find&&tailAccess.read&&tailAccess.write;}
inline void bindTailAccess(const TailAccess& access){tailAccess=access;}
inline void clearTailAccess(){tailAccess=TailAccess{};}
inline bool tailReadThunk(void* context,void* table,uint64_t key,uint64_t* value,bool* found){
    if(!tailAccess.read)return false;
    return tailAccess.read(context,table,key,value,found);
}
/* Both are defined below; restoring a record needs them. */
inline int migrateTailConfig(Binding& binding,const component_tail::Stored& stored,
                            std::vector<uint8_t>& out);
inline int publishTailConfig(Binding& binding,uint32_t schema,const void* data,uint32_t bytes);
/* Installs the configuration a record already carries, if it carries one.  A
   record that is present but does not fit this definition is offered to the
   definition's migration; without one - or when the migration keeps or refuses
   it - the record keeps its bytes and the instance runs on the registered
   default.  The bytes are never overwritten on a failed upgrade: they are what
   a later, correct migration needs. */
inline void restoreTailConfig(Binding& binding){
    if(binding.tailProbed)return;
    if(!tailAccessAvailable()){
        binding.tailProbed=true;
        return;
    }
    if(binding.config.empty()||binding.config.size()>component_tail::kMaxConfigBytes){
        binding.tailProbed=true;
        return;
    }
    void* table=nullptr;
    if(!tailAccess.find(tailAccess.context,binding.definition->api.custom_id,binding.instance,&table)||!table)
        return;  /* the record is not on the current Board (yet): retry later */
    binding.tailBound=true;
    binding.tailProbed=true;
    component_tail::Stored stored;
    const auto status=component_tail::readStored(&tailReadThunk,tailAccess.context,table,
                                                 binding.definition->api.custom_id,
                                                 component_tail::kMaxConfigBytes,&stored);
    if(status==component_tail::Status::Missing){
        note("instance "+hex64(binding.instance)+" has no stored configuration");
        return;
    }
    if(status!=component_tail::Status::Ok){
        note("instance "+hex64(binding.instance)+" kept its stored configuration: "+
             component_tail::statusName(status));
        return;
    }
    const uint32_t expected=static_cast<uint32_t>(binding.config.size());
    if(stored.schema==binding.definition->configSchema&&stored.bytes==expected){
        binding.config=std::move(stored.data);
        binding.tailRecord=true;
        binding.loadedFromRecord=true;
        note("instance "+hex64(binding.instance)+" restored "+
             std::to_string(binding.config.size())+" configuration byte(s) from its saved record");
        /* When the record only becomes reachable after the instance was created -
           a compile works on a flattened sequence, so the Board lookup can fail
           during binding - the load notification belongs here instead of in the
           create window.  The callback runs with the binding lock held, like
           on_create, so it must not call the services. */
        if(binding.created)notifyLoad(binding);
        return;
    }
    /* The circuit carries a configuration this definition cannot use as it
       stands.  A definition that declares a migration gets the bytes; everyone
       else keeps the record and runs on the registered default, which is what
       the record's own checksum already proved to be intact. */
    std::vector<uint8_t> converted(expected,0);
    const int result=migrateTailConfig(binding,stored,converted);
    if(result==TC_COMPONENT_CONFIG_MIGRATE_OK){
        const int published=publishTailConfig(binding,binding.definition->configSchema,
                                              converted.data(),expected);
        if(published!=TC_COMPONENT_STORAGE_OK){
            note("instance "+hex64(binding.instance)+
                 " was migrated but the upgraded record could not be stored; the instance runs on its default configuration");
            return;
        }
        binding.config=std::move(converted);
        binding.loadedFromRecord=true;
        note("instance "+hex64(binding.instance)+" migrated its stored configuration from schema "+
             std::to_string(stored.schema)+" ("+std::to_string(stored.bytes)+" bytes) to schema "+
             std::to_string(binding.definition->configSchema)+" ("+std::to_string(expected)+" bytes)");
        if(binding.created)notifyLoad(binding);
        return;
    }
    note("instance "+hex64(binding.instance)+
         (result==TC_COMPONENT_CONFIG_MIGRATE_KEEP?" kept":" refused to upgrade")+
         " a stored configuration written under schema "+std::to_string(stored.schema)+
         " ("+std::to_string(stored.bytes)+" bytes); the record is unchanged and the instance runs on its default configuration");
}
/* A compile can bind a custom instance before the new Board exposes that
   instance's tail table.  Binding deliberately leaves tailProbed clear in that
   case, but configuration restoration must not depend on a later UI/service
   read: a source component has to start producing its saved value even when it
   is never selected.  The game-thread frame calls this after observing the
   Board, retrying only the small set of live, still-unprobed bindings. */
inline size_t restorePendingTailConfigs(){
    std::vector<std::shared_ptr<Binding>> pending;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        for(const auto& binding:bindings)
            if(binding&&binding->active&&!binding->tailProbed)pending.push_back(binding);
    }
    size_t completed=0;
    for(const auto& binding:pending){
        std::lock_guard<std::mutex> lock(binding->mutex);
        const bool before=binding->tailProbed;
        restoreTailConfig(*binding);
        if(!before&&binding->tailProbed)++completed;
    }
    return completed;
}
/* Asks the definition to convert a stored configuration it cannot use.  A
   definition without a migration - or one that answers with an unknown code -
   is reported as a refusal, so a broken callback can never be mistaken for a
   successful upgrade. */
inline int migrateTailConfig(Binding& binding,const component_tail::Stored& stored,
                            std::vector<uint8_t>& out){
    const auto* definition=binding.definition.get();
    if(!definition->migrateConfig||
       definition->migrationVersion!=TC_COMPONENT_CONFIG_MIGRATION_VERSION_1)
        return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    const int result=definition->migrateConfig(definition->migrationUser,stored.schema,
                                               stored.data.data(),stored.bytes,
                                               definition->configSchema,out.data(),
                                               static_cast<uint32_t>(out.size()));
    if(result==TC_COMPONENT_CONFIG_MIGRATE_OK||result==TC_COMPONENT_CONFIG_MIGRATE_KEEP||
       result==TC_COMPONENT_CONFIG_MIGRATE_REJECT)
        return result;
    note("internal: migration callback for custom "+hex64(definition->api.custom_id)+
         " returned "+std::to_string(result)+"; treated as a refusal");
    return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
}
/* Publishes one configuration into the instance's record.  The entries are
   built and decoded back first, then committed chunks-first with the checksum
   last, so a commit that stops early leaves an unreadable record rather than a
   valid record with half of the new bytes in it. */
inline int publishTailConfig(Binding& binding,uint32_t schema,const void* data,uint32_t bytes){
    if(!tailAccessAvailable())return TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
    if(bytes>component_tail::kMaxConfigBytes)return TC_COMPONENT_STORAGE_ERR_SIZE;
    void* table=nullptr;
    if(!tailAccess.find(tailAccess.context,binding.definition->api.custom_id,binding.instance,&table)||!table)
        return TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
    const auto* source=static_cast<const uint8_t*>(data);
    const auto entries=component_tail::encode(binding.definition->api.custom_id,schema,source,bytes);
    if(bytes&&entries.empty())return TC_COMPONENT_STORAGE_ERR_SIZE;
    std::vector<uint8_t> check(bytes,0);
    if(component_tail::decodeEntries(entries,binding.definition->api.custom_id,schema,bytes,
                                     check.data())!=component_tail::Status::Ok){
        note("stored configuration for instance "+hex64(binding.instance)+
             " was refused by the host's own decoder; nothing written");
        return TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
    }
    for(const auto& entry:entries)
        if(!tailAccess.write(tailAccess.context,table,entry.key,entry.value)){
            binding.tailRecord=false;
            note("stored configuration for instance "+hex64(binding.instance)+
                 " could not be written; the record is left unreadable and the next write repairs it");
            return TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
        }
    binding.tailBound=true;
    binding.tailRecord=true;
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageInfo(const TCComponentInstanceHandle* handle,
                       TCComponentStorageInfoV1* out) {
    if(!out||out->size<sizeof(*out))return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    restoreTailConfig(*binding);
    TCComponentStorageInfoV1 info{};
    info.size=sizeof(info);info.version=TC_COMPONENT_STORAGE_INFO_VERSION_1;
    info.flags=(binding->config.empty()?0u:TC_COMPONENT_STORAGE_HAS_CONFIG)|
               (binding->state.empty()?0u:TC_COMPONENT_STORAGE_HAS_STATE)|
               (binding->tailBound?TC_COMPONENT_STORAGE_HAS_PERSISTENCE:0u);
    info.config_schema=binding->definition->configSchema;
    info.config_size=static_cast<uint32_t>(binding->config.size());
    info.state_size=static_cast<uint32_t>(binding->state.size()*sizeof(uint64_t));
    info.config_revision=binding->configRevision;
    info.handle=*handle;
    *out=info;
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageReadConfig(const TCComponentInstanceHandle* handle,void* out,
                             uint32_t capacity,uint32_t* bytes) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    restoreTailConfig(*binding);
    const uint32_t count=static_cast<uint32_t>(binding->config.size());
    if(bytes)*bytes=count;
    if(!out&&capacity==0)return TC_COMPONENT_STORAGE_OK;
    if(!out)return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
    if(capacity<count)return TC_COMPONENT_STORAGE_ERR_SIZE;
    if(count)std::memcpy(out,binding->config.data(),count);
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageWriteConfig(const TCComponentInstanceHandle* handle,uint32_t schema,
                              const void* data,uint32_t bytes) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::vector<uint8_t> changedConfig;
    {
        std::lock_guard<std::mutex> lock(binding->mutex);
        restoreTailConfig(*binding);
        if(schema!=binding->definition->configSchema)return TC_COMPONENT_STORAGE_ERR_SCHEMA;
        if(bytes!=binding->config.size())return TC_COMPONENT_STORAGE_ERR_SIZE;
        if(bytes&&!data)return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
        const bool identical=bytes==0||std::memcmp(binding->config.data(),data,bytes)==0;
        /* The record is the durable copy, so it is written first: a write that
           cannot reach the record leaves the in-memory configuration untouched
           instead of making the two disagree. */
        if(binding->tailBound&&!(identical&&binding->tailRecord)){
            const int published=publishTailConfig(*binding,schema,data,bytes);
            if(published!=TC_COMPONENT_STORAGE_OK)return published;
        }
        if(!identical){
            /* One write is one undo step - unless a tool has opened a transaction,
               in which case the whole span becomes one step when it commits. */
            if(!binding->editOpen){
                ConfigEdit edit;
                edit.customId=binding->definition->api.custom_id;
                edit.instance=binding->instance;
                edit.schema=binding->definition->configSchema;
                edit.before=binding->config;
                edit.after.assign(static_cast<const uint8_t*>(data),
                                  static_cast<const uint8_t*>(data)+bytes);
                configUndoStack.push_back(std::move(edit));
                configRedoStack.clear();
                trimConfigEdits();
            }
            std::memcpy(binding->config.data(),data,bytes);
            ++binding->configRevision;
            if(!binding->configRevision)binding->configRevision=1;
            changedConfig=binding->config;
        }
    }
    /* Announced with the lock released: the callback is allowed to call the
       storage services, and those take the same lock. */
    if(!changedConfig.empty()){
        notifyConfigChanged(binding,changedConfig);
    }
    return TC_COMPONENT_STORAGE_OK;
}
/* ---- grouped configuration edits (tc.component.storage V2) -----------------
   Plan §9.3: a batch tool says where the player-visible action begins and ends
   instead of letting the host guess from frame boundaries.  `begin_edit` keeps the
   bytes the configuration has at that moment; the writes in between change it but
   produce no undo step; `commit_edit` turns the span into one step, `abort_edit`
   puts the bytes back and touches no stack. */
inline int storageBeginEdit(const TCComponentInstanceHandle* handle) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    restoreTailConfig(*binding);
    if(binding->editOpen)return TC_COMPONENT_STORAGE_ERR_STATE;
    binding->editOpen=true;
    binding->editBefore=binding->config;
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageCommitEdit(const TCComponentInstanceHandle* handle) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    if(!binding->editOpen)return TC_COMPONENT_STORAGE_ERR_STATE;
    binding->editOpen=false;
    if(binding->editBefore!=binding->config){
        ConfigEdit edit;
        edit.customId=binding->definition->api.custom_id;
        edit.instance=binding->instance;
        edit.schema=binding->definition->configSchema;
        edit.before=binding->editBefore;
        edit.after=binding->config;
        configUndoStack.push_back(std::move(edit));
        configRedoStack.clear();
        trimConfigEdits();
    }
    binding->editBefore.clear();
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageAbortEdit(const TCComponentInstanceHandle* handle) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::vector<uint8_t> restored;
    {
        std::lock_guard<std::mutex> lock(binding->mutex);
        if(!binding->editOpen)return TC_COMPONENT_STORAGE_ERR_STATE;
        binding->editOpen=false;
        restored=std::move(binding->editBefore);
        binding->editBefore.clear();
        if(restored==binding->config)return TC_COMPONENT_STORAGE_OK;
        if(binding->tailBound){
            const int published=publishTailConfig(*binding,binding->definition->configSchema,
                                                  restored.data(),
                                                  static_cast<uint32_t>(restored.size()));
            if(published!=TC_COMPONENT_STORAGE_OK)return published;
        }
        binding->config=restored;
        ++binding->configRevision;
        if(!binding->configRevision)binding->configRevision=1;
    }
    notifyConfigChanged(binding,restored);
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageCaptureState(const TCComponentInstanceHandle* handle,void* out,
                               uint32_t capacity,uint32_t* bytes) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    const uint32_t count=static_cast<uint32_t>(binding->state.size()*sizeof(uint64_t));
    if(bytes)*bytes=count;
    if(!out&&capacity==0)return TC_COMPONENT_STORAGE_OK;
    if(!out)return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
    if(capacity<count)return TC_COMPONENT_STORAGE_ERR_SIZE;
    if(count)std::memcpy(out,binding->state.data(),count);
    return TC_COMPONENT_STORAGE_OK;
}
inline int storageRestoreState(const TCComponentInstanceHandle* handle,const void* data,
                               uint32_t bytes) {
    std::shared_ptr<Binding> binding;
    if(!instanceMatches(handle,binding))return TC_COMPONENT_STORAGE_ERR_STALE;
    std::lock_guard<std::mutex> lock(binding->mutex);
    const uint32_t expected=static_cast<uint32_t>(binding->state.size()*sizeof(uint64_t));
    if(bytes!=expected)return TC_COMPONENT_STORAGE_ERR_SIZE;
    if(bytes&&!data)return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
    if(bytes)std::memcpy(binding->state.data(),data,bytes);
    return TC_COMPONENT_STORAGE_OK;
}
using AddCircuit=void(*)(void*,void*,uint8_t,void*,void*,void*);
using AddLine=void(*)(const TCNimString*,void*);
using CompileRun=void(*)(void*,void*,void*);
inline AddCircuit originalCircuit=nullptr;
inline AddLine originalLine=nullptr;
inline CompileRun originalCompile=nullptr;
using JitRun=void(*)(void*,int64_t);
inline JitRun originalJit=nullptr;
struct Emission {
    // One entry per node that will be replaced, keyed by its node index.
    struct Role {
        uint64_t token=0;
        uint32_t index=0;
        bool first=false;
        bool collector=false;
        bool triState=false;
        bool clockedTick=false;
    };
    // Per compiled instance: the shape the definition declares.
    struct Instance {
        uint64_t custom=0;                 /* the prototype this instance refers to */
        uint32_t inputs=0,outputs=0;
        std::array<uint32_t,kMaxBridgeInputs> widths{};
        std::array<uint32_t,kMaxBridgeOutputs> outWidths{};
        std::vector<std::string> operands;
        /* One scalar per input: 1 means the whole pin is driven, 0 means Z.
           The pinned game exposes one Z flag for a word net, not one per bit. */
        std::vector<std::string> drives;
        bool v3=false;
        bool generated=false;
        bool preciseDependencies=false;
        bool clockedTick=false;
        std::vector<uint32_t> outputDependencies;
        std::vector<std::string> inlinePrefixes;
        std::vector<std::string> inlineExpressions;
        /* One shared SSA prefix per dependency group and host phase.  Canonical
           declarations can also occur in several different groups; keep their
           names so the first actual schedule point becomes the sole definition. */
        std::set<uint64_t> inlineGroups;
        std::set<std::string> inlineDeclarations;
        bool ready=false,failed=false;
        std::set<uint64_t> warned;
    };
    struct DynamicConstant {uint64_t component=0,value=0,width=0;};
    std::unordered_map<uint64_t,Role> nodes;
    std::map<uint64_t,Instance> instances;
    std::unordered_map<uint64_t,DynamicConstant> constants;
    std::set<uint64_t> drivenNets;
    std::set<uint64_t> refreshDrivenNets;
    std::set<uint64_t> refreshNetCandidates;
    Role current{};
    bool currentValid=false;
    Role triRole{};
    bool triRewriteValid=false;
    bool triEnableDone=false;
    uint32_t triValueWrites=0;
    uint32_t triSkipDepth=0;
    std::string triNet;
    bool triRefresh=false;
    bool triPendingRefresh=false;
    DynamicConstant currentConstant{};
    bool currentConstantValid=false;
    uint32_t mode=0;
    size_t rewritten=0;
};
inline thread_local Emission* emission=nullptr;
inline bool layoutLogged=false;
inline std::set<uint64_t> reportedMissing;
// Logic nodes inside a component's internal circuit.  0x03..0x0b are the 1-bit
// gates (NOT/AND/NAND/OR/NOR/XOR/XNOR and the three-input variants); 0x12..0x1e
// are the combinational word components (the same gates plus increment, add,
// negate, equal and friends) and 0x2a is the word Mux.  Their parent field
// points at the owning custom instance, while an instance's pin helpers use
// kind 0x50.
inline bool logicGateKind(uint8_t kind) {
    return (kind>=0x03&&kind<=0x0c)||(kind>=0x12&&kind<=0x1e)||kind==0x2a;
}
inline size_t matchParen(const std::string& text,size_t open) {
    int depth=0;
    for(size_t i=open;i<text.size();++i) {
        if(text[i]=='(')++depth;
        else if(text[i]==')'&&--depth==0)return i;
    }
    return std::string::npos;
}
// Operand tuple of a generated value expression, in evaluation order:
//   run mode     "U1 (U1 vid262) & (U1 vid260)"        -> one reference per net
//   refresh mode "U1 (load(<U1>, #STATE + 262)) & ..." -> one load(...) each
// Unary gates such as NOT are handled too, because only the referenced leaves
// are collected.
inline std::vector<std::string> operandTuple(const std::string& text,size_t begin,bool refresh) {
    std::vector<std::string> operands;
    for(size_t i=begin;i<text.size();) {
        if(refresh) {
            if(text.compare(i,5,"load(")!=0){++i;continue;}
            const size_t close=matchParen(text,i+4);
            if(close==std::string::npos)break;
            operands.push_back(text.substr(i,close-i+1));
            i=close+1;
            continue;
        }
        const bool valueId=text.compare(i,8,"value_id")==0;
        const bool vid=text.compare(i,3,"vid")==0;
        if(!valueId&&!vid){++i;continue;}
        size_t j=i+(valueId?8:3);
        const size_t firstDigit=j;
        while(j<text.size()&&text[j]>='0'&&text[j]<='9')++j;
        if(j==firstDigit){++i;continue;}
        // Bare variable reference: the declaration already carries the pin's
        // width, and the packed expression casts to U64 explicitly.
        operands.push_back(text.substr(i,j-i));
        i=j;
    }
    return operands;
}
inline void observeDriveNets(Emission& context,const std::string& text) {
    for(const char* marker:{"let zid","var zid"}) {
        for(size_t pos=0;(pos=text.find(marker,pos))!=std::string::npos;++pos) {
            size_t first=pos+std::strlen(marker),end=first;
            while(end<text.size()&&text[end]>='0'&&text[end]<='9')++end;
            if(end>first)context.drivenNets.insert(std::strtoull(text.substr(first,end-first).c_str(),nullptr,10));
            pos=end;
        }
    }
    /* In refresh code a resolved tri-state net is persisted as value slot N
       and drive slot N-1.  `var vidN` and its AND accumulator may arrive in
       separate add_line calls, so remember candidates until both are seen. */
    for(size_t pos=0;(pos=text.find("var vid",pos))!=std::string::npos;++pos) {
        size_t first=pos+7,end=first;
        while(end<text.size()&&text[end]>='0'&&text[end]<='9')++end;
        if(end==first){pos=end;continue;}
        const uint64_t id=std::strtoull(text.substr(first,end-first).c_str(),nullptr,10);
        context.refreshNetCandidates.insert(id);
        pos=end;
    }
    for(size_t pos=0;(pos=text.find("and_field_id",pos))!=std::string::npos;++pos) {
        size_t first=pos+12,end=first;
        while(end<text.size()&&text[end]>='0'&&text[end]<='9')++end;
        if(end>first) {
            const uint64_t id=std::strtoull(text.substr(first,end-first).c_str(),nullptr,10);
            if(context.refreshNetCandidates.count(id))context.refreshDrivenNets.insert(id);
        }
        pos=end;
    }
}
inline std::string driveOperand(const Emission& context,const std::string& operand,bool refresh) {
    if(refresh) {
        const std::string marker="#SIMULATION_STATE + ";
        const size_t pos=operand.find(marker);
        if(pos!=std::string::npos) {
            size_t first=pos+marker.size(),end=first;
            while(end<operand.size()&&operand[end]>='0'&&operand[end]<='9')++end;
            if(end>first) {
                const uint64_t id=std::strtoull(operand.substr(first,end-first).c_str(),nullptr,10);
                if(context.refreshDrivenNets.count(id)&&id)
                    return "load(<U1>, #SIMULATION_STATE + "+std::to_string(id-1)+")";
            }
        }
        return "1";
    }
    if(operand.rfind("vid",0)==0) {
        const uint64_t id=std::strtoull(operand.c_str()+3,nullptr,10);
        if(context.drivenNets.count(id))return "zid"+std::to_string(id);
    }
    return "1";
}

inline bool outputGroupLeader(const std::vector<uint32_t>& dependencies,uint32_t output) {
    if(output>=dependencies.size())return true;
    for(uint32_t prior=0;prior<output;++prior)
        if(dependencies[prior]==dependencies[output])return false;
    return true;
}

inline void circuit(void* a,void* b,uint8_t c,void* d,void* e,void* f) {
    Emission context;
    context.mode=c;
    /* What this compile contains, so instances that are gone can be released
       once the present ones are bound. */
    std::set<std::pair<uint64_t,uint64_t>> survivingKeys;
    /* Newly bound instances are announced after the registry lock is dropped:
       a lifecycle callback is plugin code and may call back into a service that
       takes the same lock. */
    std::vector<std::shared_ptr<Binding>> createdBindings;
    /* Every instance that is part of this compile (new or already bound): the
       TC_LOGIC_COMPILE notification goes to all of them once the generator has
       produced the code below. */
    std::vector<std::shared_ptr<Binding>> compiledBindings;
    // The code generator receives the flattened component sequence as arg 2.
    const auto count=read64(b,0),data=read64(b,8);
    if(count<=1000000 && data) {
        auto node=[&](size_t i){return reinterpret_cast<const unsigned char*>(data+8+i*0x238);};
        std::unordered_map<uint64_t,size_t> parents;
        /* The compile walk also learns which component each custom instance
           belongs to: record +0x18 is the id the game addresses when it is
           selected or walked from the top, and every component placed directly
           on the board carries its own id there.  This map is what
           tc.component.instances V2 answers from (and what the diagnostic file
           below prints). */
        std::vector<ComponentNode> customs;
        for(size_t i=0;i<count;++i)
            if(node(i)[0]==0x4e) {
                const uint64_t id=read64(node(i),8);
                parents.emplace(id,i);
                customs.push_back(ComponentNode{0x4e,id,read64(node(i),0x18)});
            }
        const auto containers=containerMap(customs);
        for(size_t i=0;i<count;++i) {
            const auto* component=node(i);
            if(component[0]!=0x2e)continue;
            const uint64_t settingCount=read64(component,0xa8);
            const uint64_t settings=read64(component,0xb0);
            const uint64_t width=read64(component,0xe0);
            if(!settingCount||!settings||width<=8||width>64)continue;
            const uint64_t id=read64(component,8);
            const uint64_t value=read64(reinterpret_cast<const void*>(settings),8);
            context.constants.emplace(i,Emission::DynamicConstant{id,value,width});
            setDynamicConstant(id,value);
        }
        if(!layoutLogged && !parents.empty() && logger) {
            layoutLogged=true;
            std::ostringstream text;
            // One-shot dump of the flattened board, useful when a component
            // is not bridged and its internal nodes have to be inspected.
            text<<"Native logic: board layout";
            for(size_t i=0;i<count;++i) {
                text<<" node"<<i<<"=0x"<<std::hex<<static_cast<unsigned>(node(i)[0])<<std::dec
                    <<"/parent=0x"<<std::hex<<read64(node(i),0x18)<<std::dec;
            }
            logger(text.str());
        }
        /* The game's selection set is keyed by a component's top-level id while
           the bridge hands a callback the instance id; a Mod that wants to react
           to a click on its own component needs both.  Publish the pairs next to
           the game (the working directory the source dumps use) so it can match
           them.  Per compile, so it follows the board. */
        {
            std::ofstream mapping("interactive-map.txt", std::ios::trunc);
            if(mapping){
                /* Node order, like before: the file is a diagnostic a human
                   reads, and a stable order keeps two runs diffable. */
                for(size_t i=0;i<count;++i){
                    if(node(i)[0]!=0x4e)continue;
                    const uint64_t id=read64(node(i),8);
                    const auto found=containers.find(id);
                    mapping<<std::hex<<id<<" "<<(found==containers.end()?id:found->second)<<"\n";
                }
            }
        }
        // A custom instance is bridged when its internal gates match the shape
        // the definition declares: one logic node per output pin.  Those lines
        // are the component's whole behaviour, so replacing them keeps every
        // other component and wire under the game's own code generator.
        std::map<size_t,std::vector<size_t>> logicNodes;
        for(size_t i=0;i<count;++i) {
            const uint8_t kind=node(i)[0];
            if(!logicGateKind(kind))continue;
            auto parent=parents.find(read64(node(i),0x18));if(parent==parents.end())continue;
            logicNodes[parent->second].push_back(i);
        }
        std::lock_guard<std::mutex> lock(registryMutex);
        for(auto& entry:logicNodes) {
            const size_t parentIndex=entry.first;
            std::vector<size_t>& nodesOfInstance=entry.second;
            auto custom=read64(node(parentIndex),0x188);
            auto def=definitions.find(custom);if(def==definitions.end()||!def->second->active)continue;
            auto instance=read64(node(parentIndex),8);
            // Every output pin of the component needs one gate driving it, and
            // those gates are emitted before their consumers.
            const bool generated=def->second->api.version>=3;
            // The legacy generated scaffold has one all-input chain.  A precise
            // scaffold has one collector per input followed by the private
            // dependency gates of every output cone, then independent drivers.
            const bool precise=generated&&!def->second->outputDependencies.empty();
            size_t dependencyGates=0;
            if(precise)for(uint32_t mask:def->second->outputDependencies) {
                unsigned count=0;
                while(mask){mask&=mask-1;++count;}
                if(count>1)dependencyGates+=count-1;
            }
            if(precise&&def->second->clockedTick&&def->second->inputs>1)
                dependencyGates+=def->second->inputs-1;
            const size_t prefix=generated?
                (precise?def->second->inputs+dependencyGates:
                         (def->second->inputs?2*def->second->inputs-1:0)):0;
            const size_t expected=prefix+def->second->outputs+
                                  ((precise&&def->second->clockedTick)?1:0)+
                                  ((generated&&def->second->outputs==0)?1:0);
            if(nodesOfInstance.size()!=expected) {
                note("instance "+hex64(instance)+" of custom "+hex64(custom)+" has "+
                     std::to_string(nodesOfInstance.size())+" internal logic node(s) for "+
                     std::to_string(def->second->outputs)+" output(s); internal circuit kept");
                continue;
            }
            auto key=std::make_pair(custom,instance);
            auto found=bindingKeys.find(key);
            /* The game's own top-level id for this instance, learned by this
               compile's walk: `tc.component.instances` V2 hands it back so a
               Mod never has to parse the diagnostic file. */
            const auto container=containers.find(instance);
            const uint64_t topLevel=container==containers.end()?instance:container->second;
            uint64_t token;
            if(found!=bindingKeys.end()){
                token=found->second;
                const size_t existing=static_cast<size_t>(token-1);
                if(existing<bindings.size()&&bindings[existing])
                    bindings[existing]->topLevel=topLevel;
            }
            else {auto binding=std::make_shared<Binding>();binding->definition=def->second;binding->instance=instance;
                binding->topLevel=topLevel;
                /* V1 definitions keep the historical eight state words; a V2
                   definition says how many it wants (0 = stateless). */
                binding->state.assign((def->second->useV2||def->second->useV3)?def->second->stateWords:8,0);
                binding->refreshState.assign(binding->state.size(),0);
                binding->config=def->second->defaultConfig;
                /* A duplicated instance starts from the source's configuration
                   (the copy is already in its record) and says so on this bind. */
                const auto clone=pendingClones.find(std::make_pair(def->second->api.custom_id,
                                                                  instance));
                if(clone!=pendingClones.end()){
                    binding->clonePending=true;
                    if(clone->second.size()==binding->config.size())
                        binding->config=clone->second;
                    pendingClones.erase(clone);
                }
                /* The circuit file is the authority for a configuration that
                   was saved with the component; the registered default only
                   applies when the record carries nothing this build can use. */
                restoreTailConfig(*binding);
                binding->generation=instanceGeneration.fetch_add(1)+1;
                /* A released instance keeps its slot so the tokens of the others
                   do not move; reuse is what a stale handle notices. */
                size_t slot=bindings.size();
                for(size_t i=0;i<bindings.size();++i)
                    if(bindings[i]&&!bindings[i]->active){slot=i;break;}
                if(slot==bindings.size())bindings.push_back(binding);
                else bindings[slot]=binding;
                token=slot+1;bindingKeys.emplace(key,token);
                createdBindings.push_back(binding);
                note("bound instance "+hex64(instance)+" of custom "+hex64(custom)+" as token "+std::to_string(token));}
            survivingKeys.emplace(key);
            std::sort(nodesOfInstance.begin(),nodesOfInstance.end());
            for(size_t k=0;k<nodesOfInstance.size();++k) {
                if(generated && k<def->second->inputs)
                    context.nodes.emplace(nodesOfInstance[k],Emission::Role{
                        token,static_cast<uint32_t>(k),false,true,false,false});
                else if(k>=prefix&&k<prefix+def->second->outputs) {
                    const uint32_t output=static_cast<uint32_t>(k-prefix);
                    context.nodes.emplace(nodesOfInstance[k],Emission::Role{
                        token,output,precise?
                            (def->second->groupedOutputs?
                                outputGroupLeader(def->second->outputDependencies,output):true):
                            k==prefix,false,
                        def->second->useV3,false});
                }
                else if(precise&&def->second->clockedTick&&
                        k==prefix+def->second->outputs)
                    context.nodes.emplace(nodesOfInstance[k],Emission::Role{
                        token,0,true,false,false,true});
            }
    auto& state=context.instances[token];
    state.generated=generated;
    state.custom=custom;
            if(generated){state.operands.resize(def->second->inputs);state.drives.resize(def->second->inputs);}
            state.v3=def->second->useV3;
            state.preciseDependencies=precise;
            state.clockedTick=def->second->clockedTick;
            state.outputDependencies=def->second->outputDependencies;
            state.inlinePrefixes=def->second->inlinePrefixes;
            state.inlineExpressions=def->second->inlineExpressions;
            state.inputs=def->second->inputs;state.outputs=def->second->outputs;
            state.widths=def->second->inputWidths;
            state.outWidths=def->second->outputWidths;
        }
        // Instances whose internals we could not recognise at all are reported
        // once, so a silent "no callback ran" is avoidable.
        for(size_t i=0;i<count;++i) {
            if(node(i)[0]!=0x4e)continue;
            const size_t parentIndex=i;
            if(logicNodes.count(parentIndex))continue;
            auto custom=read64(node(parentIndex),0x188);
            auto def=definitions.find(custom);
            if(def==definitions.end()||!def->second->active)continue;
            if(reportedMissing.insert(custom).second)
                note("instance "+hex64(read64(node(parentIndex),8))+" of custom "+hex64(custom)+
                     " has no recognised internal logic node; internal circuit kept (see board layout log)");
        }
    }
 /* A compile that contains at least one of our instances *is* the board, so
    anything of ours missing from it has been deleted (the level's own test
    program compiles too, and it contains none of them - releasing on that
    would tear down live instances on every level load). */
 if(!survivingKeys.empty()) releaseInstances(&survivingKeys,"board recompiled");
    {
        /* The instances this compile carries, looked up after the release above
           so a key that did not survive cannot be announced. */
        std::lock_guard<std::mutex> lock(registryMutex);
        for(const auto& key:survivingKeys) {
            const auto found=bindingKeys.find(key);
            if(found==bindingKeys.end())continue;
            const size_t slot=static_cast<size_t>(found->second);
            if(slot==0||slot>bindings.size()||!bindings[slot-1]||!bindings[slot-1]->active)continue;
            compiledBindings.push_back(bindings[slot-1]);
        }
    }
    for(auto& binding:createdBindings) {
        std::lock_guard<std::mutex> lock(binding->mutex);
        notifyCreate(*binding);
        /* A duplicated instance learns about it after its normal create, with the
           copied configuration already in place. */
        if(binding->clonePending){
            binding->clonePending=false;
            notifyClone(*binding);
        } else if(binding->loadedFromRecord){
            notifyLoad(*binding);
        }
    }
    auto* previous=emission;emission=&context;
    originalCircuit(a,b,c,d,e,f);
    emission=previous;
    /* The generator has produced this compile's code, so every instance that was
       part of it hears TC_LOGIC_COMPILE - after its create/clone/load, so a
       callback can rely on the instance being fully bound. */
    for(auto& binding:compiledBindings) {
        std::lock_guard<std::mutex> lock(binding->mutex);
        notifyCompile(*binding);
    }
    if(context.rewritten && logger)logger("Native logic: emitted "+std::to_string(context.rewritten)+" callback(s), mode="+std::to_string(c));
}
// Word-width result of an output pin.  Foreign call results survive the JIT only
// as single bits, so the callback publishes its output bits into reserved
// simulation-state slots and the generated program loads and assembles them,
// mirroring the way the game's own word components read their inputs.
inline std::string wordValueExpression(const std::string& type,uint64_t token,uint32_t pin,
                                       uint32_t width) {
    std::string bits;
    for(uint32_t bit=0;bit<width&&bit<64;++bit) {
        std::string term="((("+type+" (load(<U1>, #SIMULATION_STATE + "+
                         std::to_string(stateSlot(token,pin,bit))+")))) & 1)";
        if(bit)term="("+term+" << "+std::to_string(bit)+")";
        if(bit)bits+=" | ";
        bits+=term;
    }
    return type+" ("+bits+")";
}
inline std::string wordAssembly(const std::string& type,uint64_t token,uint32_t pin,
                               uint32_t width,const std::string& prefix) {
    return prefix+wordValueExpression(type,token,pin,width);
}
inline void replaceAll(std::string& text,const std::string& marker,
                       const std::string& replacement) {
    size_t at=0;
    while((at=text.find(marker,at))!=std::string::npos) {
        text.replace(at,marker.size(),replacement);
        at+=replacement.size();
    }
}
inline std::string instantiateInline(const Emission::Instance& instance,uint64_t token,
                                     uint32_t output,bool prefix) {
    const auto& programs=prefix?instance.inlinePrefixes:instance.inlineExpressions;
    if(output>=programs.size())return {};
    std::string result=programs[output];
    replaceAll(result,"@T@",std::to_string(token));
    for(uint32_t input=0;input<instance.inputs;++input)
        replaceAll(result,"@"+std::to_string(input)+"@",
                   input<instance.operands.size()&&!instance.operands[input].empty()?
                       instance.operands[input]:"U64 0");
    return result;
}
inline std::string instantiateInlinePrefixOnce(Emission::Instance& instance,uint64_t token,
                                                uint32_t output,bool refresh) {
    const uint32_t mask=output<instance.outputDependencies.size()?
        instance.outputDependencies[output]:~uint32_t(0);
    const uint64_t key=uint64_t(mask)|(uint64_t(refresh?1u:0u)<<32);
    if(!instance.inlineGroups.insert(key).second)return {};
    const std::string prefix=instantiateInline(instance,token,output,true);
    std::string result;
    size_t begin=0;
    while(begin<prefix.size()) {
        size_t end=prefix.find("\nvar ",begin);
        if(end==std::string::npos)end=prefix.size();
        else ++end; /* The newline belongs to the previous declaration block. */
        const std::string block=prefix.substr(begin,end-begin);
        const size_t firstEnd=block.find('\n');
        const std::string line=block.substr(0,firstEnd);
        bool emit=true;
        if(line.rfind("var ",0)==0) {
            const size_t nameEnd=line.find(' ',4);
            if(nameEnd!=std::string::npos) {
                const std::string declaration=(refresh?"r:":"c:")+line.substr(4,nameEnd-4);
                emit=instance.inlineDeclarations.insert(declaration).second;
            }
        }
        if(emit&&!block.empty()) {
            result+=block;
            if(result.back()!='\n')result+='\n';
        }
        begin=end;
    }
    return result;
}
using PayloadExpressions = std::array<std::string,kMaxInputWords>;
inline PayloadExpressions packedExpressions(const Emission::Instance& instance,
                                             bool drivePlane,
                                             uint32_t dependencyMask=~uint32_t(0)) {
    PayloadExpressions payload;
    std::array<uint32_t,kMaxBridgeInputs> word{},shift{};
    placeInputs(instance.widths,static_cast<uint32_t>(instance.operands.size()),word,shift);
    for(size_t i=0;i<instance.operands.size();++i) {
        if(i<32&&(dependencyMask&(uint32_t(1)<<static_cast<uint32_t>(i)))==0)continue;
        std::string source=instance.operands[i];
        if(drivePlane) {
            source=i<instance.drives.size()&&!instance.drives[i].empty()?instance.drives[i]:"1";
            const uint32_t bits=instance.widths[i];
            const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
            source="(U64 ("+source+") * U64 "+hex64(mask)+")";
        }
        std::string term="U64 ("+source+")";
        if(shift[i])term="("+term+" << "+std::to_string(shift[i])+")";
        payload[word[i]]+=(payload[word[i]].empty()?"":" | ")+term;
        if(shift[i]+instance.widths[i]>64) {
            const std::string high="(U64 ("+source+") >> "+std::to_string(64-shift[i])+")";
            payload[word[i]+1]+=(payload[word[i]+1].empty()?"":" | ")+high;
        }
    }
    return payload;
}
inline uint32_t payloadWordCount(const Emission::Instance& instance,
                                 uint32_t dependencyMask=~uint32_t(0)) {
    uint32_t bit=0,highest=0;
    for(uint32_t i=0;i<instance.inputs;++i) {
        const uint32_t end=bit+instance.widths[i];
        if(i<32&&(dependencyMask&(uint32_t(1)<<i))!=0)highest=end;
        bit=end;
    }
    return (highest+kInputWordBits-1)/kInputWordBits;
}
inline std::string invokeExpression(uint64_t token,bool refresh,
                                    const PayloadExpressions& payload,bool tickOnly=false) {
    const std::string cycleArg="cycle"+std::string(refresh?"":" + 1");
    std::ostringstream call;
    call<<"game_engine.'"<<(refresh?"tc_logic_peek":
                             (tickOnly?"tc_logic_tick":"tc_logic_invoke"))
        <<"'(U64 "<<token<<", U64 ("<<cycleArg<<"), "
        <<(payload[0].empty()?"U64 0":payload[0])<<", "
        <<(payload[1].empty()?"U64 0":payload[1])<<")";
    return call.str();
}
inline std::string invokeOutputExpression(uint64_t token,bool refresh,uint32_t output,
                                          const PayloadExpressions& payload) {
    const std::string cycleArg="cycle"+std::string(refresh?"":" + 1");
    std::ostringstream call;
    call<<"game_engine.'tc_logic_output'(U64 "<<token
        <<", U64 ((("<<cycleArg<<") + 1) * 32 + "<<output<<"), "
        <<(payload[0].empty()?"U64 0":payload[0])<<", "
        <<(payload[1].empty()?"U64 0":payload[1])<<")";
    return call.str();
}
inline std::string latchDriveStatement(uint64_t token,
                                       const PayloadExpressions& drivePayload,
                                       const std::string& suffix={}) {
    return "var tc_zin"+std::to_string(token)+suffix+
           " = U1 game_engine.'tc_logic_drive_in'(U64 "+
           std::to_string(token)+", "+(drivePayload[0].empty()?"U64 0":drivePayload[0])+", "+
           (drivePayload[1].empty()?"U64 0":drivePayload[1])+")\n";
}
inline std::string stageStatements(uint64_t token,bool refresh,
                                   const PayloadExpressions& payload,uint32_t words,
                                   bool drivePlane,const std::string& suffix={}) {
    const uint32_t chunks=(words+kInputWordsPerCall-1)/kInputWordsPerCall;
    const std::string cycleArg="cycle"+std::string(refresh?"":" + 1");
    const char* function=drivePlane?"tc_logic_drive_stage":"tc_logic_stage";
    const char plane=drivePlane?'d':'v';
    std::string result;
    for(uint32_t chunk=0;chunk<chunks;++chunk) {
        const uint32_t lo=chunk*kInputWordsPerCall,hi=lo+1;
        result+="var tc_is"+std::to_string(token)+suffix+plane+(refresh?'r':'c')+
                std::to_string(chunk)+" = U1 game_engine.'"+function+"'(U64 "+
                std::to_string(token)+", U64 ((("+cycleArg+") + 1) * 32 + "+
                std::to_string((refresh?16u:0u)+chunk)+"), "+
                (payload[lo].empty()?"U64 0":payload[lo])+", "+
                (payload[hi].empty()?"U64 0":payload[hi])+")\n";
    }
    return result;
}
struct InvocationText {std::string prefix,expression;};
inline bool dependencyOperandsReady(const Emission::Instance& instance,uint32_t output,
                                    bool requireDrive,bool wholeComponent=false) {
    const uint32_t mask=instance.preciseDependencies&&!wholeComponent&&
                        output<instance.outputDependencies.size()?
        instance.outputDependencies[output]:~uint32_t(0);
    if(instance.operands.size()!=instance.inputs)return false;
    if(requireDrive&&instance.drives.size()!=instance.inputs)return false;
    for(uint32_t input=0;input<instance.inputs;++input) {
        if(input<32&&(mask&(uint32_t(1)<<input))==0)continue;
        if(instance.operands[input].empty())return false;
        if(requireDrive&&instance.drives[input].empty())return false;
    }
    return true;
}
inline InvocationText invocationText(const Emission::Instance& instance,uint64_t token,
                                     bool refresh,uint32_t output=0,
                                     bool wholeComponent=false) {
    const bool independent=instance.preciseDependencies&&!wholeComponent;
    const uint32_t mask=independent&&output<instance.outputDependencies.size()?
        instance.outputDependencies[output]:~uint32_t(0);
    const auto values=packedExpressions(instance,false,mask);
    const uint32_t words=payloadWordCount(instance,mask);
    const std::string suffix=independent?"o"+std::to_string(output):
                             (wholeComponent?"t":std::string());
    if(words<=kInputWordsPerCall) {
        const std::string prefix=instance.v3?
            latchDriveStatement(token,packedExpressions(instance,true,mask),suffix):std::string();
        return {prefix,independent?
            invokeOutputExpression(token,refresh,output,values):
            invokeExpression(token,refresh,values,wholeComponent&&!refresh)};
    }
    const uint32_t chunks=(words+kInputWordsPerCall-1)/kInputWordsPerCall;
    std::string prefix=stageStatements(token,refresh,values,words,false,suffix);
    if(instance.v3)
        prefix+=stageStatements(token,refresh,packedExpressions(instance,true,mask),words,true,suffix);
    const std::string cycleArg="cycle"+std::string(refresh?"":" + 1");
    std::ostringstream call;
    if(independent)
        call<<"game_engine.'"<<(refresh?"tc_logic_output_peek_staged":"tc_logic_output_staged")
            <<"'(U64 "<<token<<", U64 ("<<cycleArg<<"), U64 "<<chunks<<", U64 "
            <<output<<")";
    else
        call<<"game_engine.'"<<(refresh?"tc_logic_peek_staged":
                                 (wholeComponent?"tc_logic_tick_staged":
                                                  "tc_logic_invoke_staged"))
            <<"'(U64 "<<token<<", U64 ("<<cycleArg<<"), U64 "<<chunks<<", U64 0)";
    return {prefix,call.str()};
}
inline void eraseGeneratedLine(std::string& text,size_t at) {
    if(at==std::string::npos)return;
    const size_t begin=at==0?0:text.rfind('\n',at-1)+1;
    const size_t newline=text.find('\n',at);
    const size_t end=newline==std::string::npos?text.size():newline+1;
    text.erase(begin,end-begin);
}
inline bool erasePendingTriStateBlock(Emission& context,std::string& text) {
    if(context.triSkipDepth==0)return false;
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='{')++context.triSkipDepth;
        else if(text[i]=='}'&&--context.triSkipDepth==0) {
            const size_t newline=text.find('\n',i);
            text.erase(0,newline==std::string::npos?text.size():newline+1);
            return false;
        }
    }
    text.clear();
    return true;
}
inline void rewriteTriStateContinuation(Emission& context,std::string& text) {
    if(!context.triRewriteValid)return;
    if(erasePendingTriStateBlock(context,text)||text.empty())return;
    auto found=context.instances.find(context.triRole.token);
    if(found==context.instances.end())return;
    const auto& instance=found->second;
    const auto& role=context.triRole;
    const uint32_t width=role.index<instance.outWidths.size()?instance.outWidths[role.index]:1;
    const std::string type="U"+std::to_string(width<=1?1:width<=8?8:width<=16?16:width<=32?32:64);
    const bool inlineOutput=role.index<instance.inlineExpressions.size();
    const std::string outputValue=inlineOutput?
        type+" ("+instantiateInline(instance,role.token,role.index,false)+")":
        (width<=1?
            "U1 game_engine.'tc_logic_out'(U64 "+std::to_string(role.token)+", U64 "+
                std::to_string(role.index)+")":
            wordValueExpression(type,role.token,role.index,width));
    const std::string vid="vid"+context.triNet;
    const std::string andField="and_field_id"+context.triNet;
    /* Inline expressions are a single, permanently driven source.  The stock
       Switch wrapper still emits the generic tri-state merge (OR, AND,
       contention check and Z comparison) around it.  In CYCLE mode publish the
       value directly and make that generic path statically unreachable. */
    if(inlineOutput&&!context.triRefresh) {
        const std::string declaration="var "+vid;
        const size_t declared=text.find(declaration);
        if(declared!=std::string::npos) {
            const size_t equal=text.find('=',declared+declaration.size());
            const size_t end=equal==std::string::npos?std::string::npos:text.find('\n',equal);
            if(equal!=std::string::npos)
                text.replace(equal+1,(end==std::string::npos?text.size():end)-(equal+1),
                             " "+outputValue);
        }
        const std::string andDeclaration="var "+andField;
        const size_t andAt=text.find(andDeclaration);
        if(andAt!=std::string::npos) {
            const size_t equal=text.find('=',andAt+andDeclaration.size());
            const size_t end=equal==std::string::npos?std::string::npos:text.find('\n',equal);
            if(equal!=std::string::npos)
                text.replace(equal+1,(end==std::string::npos?text.size():end)-(equal+1)," "+vid);
        }
        if(!context.triEnableDone) {
            const size_t search=declared==std::string::npos?0:declared+declaration.size();
            const size_t condition=text.find("if ",search);
            const size_t brace=condition==std::string::npos?std::string::npos:
                               text.find('{',condition);
            if(condition!=std::string::npos&&brace!=std::string::npos) {
                context.triEnableDone=true;
                context.triSkipDepth=1;
                eraseGeneratedLine(text,condition);
                if(erasePendingTriStateBlock(context,text)||text.empty())return;
            }
        }
        const std::string conflict="if "+vid+" > "+andField;
        const size_t conflictAt=text.find(conflict);
        if(conflictAt!=std::string::npos) {
            context.triSkipDepth=1;
            eraseGeneratedLine(text,conflictAt);
            if(erasePendingTriStateBlock(context,text)||text.empty())return;
        }
        const std::string driven="let zid"+context.triNet+" = ";
        const size_t drivenAt=text.find(driven);
        if(drivenAt!=std::string::npos) {
            const size_t begin=drivenAt+driven.size(),end=text.find('\n',begin);
            text.replace(begin,(end==std::string::npos?text.size():end)-begin,"true");
        }
        eraseGeneratedLine(text,text.find(andDeclaration));
        context.triValueWrites=2;
        return;
    }
    if(!context.triEnableDone) {
        const std::string declaration="var vid"+context.triNet;
        const size_t declared=text.find(declaration);
        const size_t search=declared==std::string::npos?0:declared+declaration.size();
        const size_t condition=text.find("if ",search);
        const size_t brace=condition==std::string::npos?std::string::npos:text.find('{',condition);
        if(condition!=std::string::npos&&brace!=std::string::npos) {
            text.replace(condition+3,brace-(condition+3),inlineOutput?"U1 1 != 0 ":
                "(U1 game_engine.'tc_logic_drive_out'(U64 "+std::to_string(role.token)+
                ", U64 "+std::to_string(role.index)+")) != 0 ");
            context.triEnableDone=true;
        }
    }
    if(context.triRefresh) {
        if(context.triValueWrites==0) {
            const size_t value=text.find("let value = ");
            if(value!=std::string::npos) {
                const size_t end=text.find('\n',value);
                text.replace(value+12,(end==std::string::npos?text.size():end)-(value+12),outputValue);
                context.triValueWrites=1;
            }
        }
    } else {
        for(const std::string& marker:{"vid"+context.triNet+" |= ",
                                      "and_field_id"+context.triNet+" &= "}) {
            if(context.triValueWrites>=2)break;
            const size_t value=text.find(marker);
            if(value==std::string::npos)continue;
            const size_t begin=value+marker.size(),end=text.find('\n',begin);
            text.replace(begin,(end==std::string::npos?text.size():end)-begin,outputValue);
            ++context.triValueWrites;
        }
    }
}

// Borrowed Nim string payload: the callee only reads/copies this line.
inline void line(const TCNimString* value,void* context) {
    if(!emission || !value || !value->data || value->length>1000000){originalLine(value,context);return;}
    std::string text(static_cast<const char*>(value->data)+8,value->length);
    observeDriveNets(*emission,text);
    auto comment=text.find("// ");
    if(comment!=std::string::npos&&emission->triRewriteValid) {
        emission->triRewriteValid=false;
        emission->triSkipDepth=0;
    }
    if(comment!=std::string::npos) {
        std::istringstream in(text.substr(comment+3));uint64_t index;
        emission->currentValid=false;
        emission->currentConstantValid=false;
        emission->triPendingRefresh=false;
        if(in>>index){
            auto it=emission->nodes.find(index);
            if(it!=emission->nodes.end()){emission->current=it->second;emission->currentValid=true;}
            auto constant=emission->constants.find(index);
            if(constant!=emission->constants.end()){
                emission->currentConstant=constant->second;
                emission->currentConstantValid=true;
            }
        }
    }
    if(emission->currentValid&&emission->current.triState&&
       text.find("store(#SIMULATION_STATE")!=std::string::npos)
        emission->triPendingRefresh=true;
    if(emission->triRewriteValid) {
        rewriteTriStateContinuation(*emission,text);
        std::vector<char> payload(8+text.size()+1);
        const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
        memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
        TCNimString rewritten{uint64_t(text.size()),payload.data()};originalLine(&rewritten,context);
        return;
    }
    if(emission->currentConstantValid) {
        const auto eq=text.find(" = ");
        const bool refresh=text.find("let value_id")!=std::string::npos;
        const bool cycle=text.find("var vid")!=std::string::npos;
        if(eq!=std::string::npos&&(refresh||cycle)) {
            const auto constant=emission->currentConstant;
            const std::string type=constant.width<=8?"U8":constant.width<=16?"U16":
                                   constant.width<=32?"U32":"U64";
            /* The pinned Windows x64 JIT's aligned load is the native relaxed
               atomic read.  Resolve and allocate only during compilation;
               edits publish to this same permanent slot without recompiling. */
            static_assert(std::atomic<uint64_t>::is_always_lock_free);
            static_assert(sizeof(std::atomic<uint64_t>)==sizeof(uint64_t));
            const auto slot=reinterpret_cast<uintptr_t>(
                dynamicConstantSlot(constant.component,constant.value));
            text=text.substr(0,eq+3)+type+" load(<U64>, Ptr "+std::to_string(slot)+
                ") // runtime constant "+std::to_string(constant.component);
            std::vector<char> payload(8+text.size()+1);
            const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
            memcpy(payload.data(),&capacity,8);
            memcpy(payload.data()+8,text.data(),text.size());
            TCNimString rewritten{uint64_t(text.size()),payload.data()};
            originalLine(&rewritten,context);
            ++emission->rewritten;
            emission->currentConstantValid=false;
            return;
        }
    }
    if(emission->currentValid) {
        const auto& role=emission->current;
        auto state=emission->instances.find(role.token);
        if(state==emission->instances.end()) {originalLine(value,context);return;}
        if(state->second.failed) {originalLine(value,context);return;}
        const auto eq=text.find(" = ");
        const bool refresh=text.find("let value_id")!=std::string::npos;
        const bool cycle=text.find("var vid")!=std::string::npos;
        if(eq!=std::string::npos && (refresh||cycle)) {
            const size_t begin=eq+3;
            auto& instance=state->second;
            if(role.collector) {
                const auto operands=operandTuple(text,begin,refresh);
                if(operands.size()!=1 || role.index>=instance.operands.size()) {
                    instance.failed=true;note("generated input collector has an invalid operand tuple: "+text);
                } else {
                    instance.operands[role.index]=operands[0];
                    if(instance.v3&&role.index<instance.drives.size())
                        instance.drives[role.index]=driveOperand(*emission,operands[0],refresh);
                }
                emission->currentValid=false;
                originalLine(value,context);return;
            }
            // The generated line carries the pin's own type (U1..U64); the
            // replacement keeps it so word-width pins stay word-width.
            std::string type="U1";
            if(auto marker=text.find("U",begin);marker!=std::string::npos&&marker<begin+4) {
                size_t end=marker+1;
                while(end<text.size()&&text[end]>='0'&&text[end]<='9')++end;
                if(end>marker+1)type=text.substr(marker,end-marker);
            }
            std::string replacement;
            const uint32_t outWidth=
                role.index<instance.outWidths.size()?instance.outWidths[role.index]:1;
            // Word-width pins use the width-derived type: the refresh line does
            // not carry a type token of its own.
            std::string pinType=type;
            if(outWidth>1)pinType="U"+std::to_string(outWidth<=8?8:outWidth<=16?16:outWidth<=32?32:64);
            if(role.clockedTick) {
                /* The hidden sink exists only to commit sequential state.  Its
                   REFRESH form is a no-op; in CYCLE it receives the complete
                   input tuple and invokes the callback once, independently of
                   every visible output driver. */
                if(refresh) {
                    replacement=text.substr(0,begin)+type+" 0";
                } else {
                    if(!dependencyOperandsReady(instance,0,instance.v3,true)) {
                        if(instance.warned.insert(role.token).second)
                            note("instance token "+std::to_string(role.token)+
                                 " does not expose the complete clocked-tick input tuple; internal circuit kept: "+text);
                        instance.failed=true;
                        originalLine(value,context);
                        return;
                    }
                    const auto invocation=invocationText(instance,role.token,false,0,true);
                    replacement=invocation.prefix+text.substr(0,begin)+type+" "+
                                invocation.expression;
                }
                text=replacement;
                std::vector<char> payload(8+text.size()+1);
                const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
                memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
                TCNimString rewritten{uint64_t(text.size()),payload.data()};
                originalLine(&rewritten,context);
                ++emission->rewritten;emission->currentValid=false;return;
            }
            if(role.triState&&text.find("var vid")!=std::string::npos) {
                const bool triRefresh=emission->triPendingRefresh;
                std::string trigger;
                if(role.index<instance.inlineExpressions.size()) {
                    if(!dependencyOperandsReady(instance,role.index,false)) {
                        instance.failed=true;
                        note("inline output is missing one of its declared operands: "+text);
                        originalLine(value,context);
                        return;
                    }
                    trigger=instantiateInlinePrefixOnce(instance,role.token,role.index,
                                                        triRefresh);
                } else if(role.first) {
                    if(!dependencyOperandsReady(instance,role.index,true)) {
                        if(instance.warned.insert(role.token).second)
                            note("instance token "+std::to_string(role.token)+
                                 " does not expose the complete V3 value/drive input tuple; internal circuit kept: "+text);
                        instance.failed=true;
                        originalLine(value,context);
                        return;
                    }
                    const auto invocation=invocationText(instance,role.token,triRefresh,
                                                         role.index);
                    trigger=invocation.prefix+
                            "var tc_io"+std::to_string(role.token)+"o"+
                            std::to_string(role.index)+" = U1 "+
                            invocation.expression+"\n";
                }
                const size_t declaration=text.find("var vid");
                size_t firstDigit=declaration+7,endDigit=firstDigit;
                while(endDigit<text.size()&&text[endDigit]>='0'&&text[endDigit]<='9')++endDigit;
                if(endDigit==firstDigit) {
                    instance.failed=true;
                    note("V3 output Switch has an unknown generated shape: "+text);
                    originalLine(value,context);
                    return;
                }
                const std::string triNet=text.substr(firstDigit,endDigit-firstDigit);
                text.insert(declaration,trigger);
                emission->triRole=role;
                emission->triRewriteValid=true;
                emission->triEnableDone=false;
                emission->triValueWrites=0;
                emission->triSkipDepth=0;
                emission->triNet=triNet;
                emission->triRefresh=triRefresh;
                rewriteTriStateContinuation(*emission,text);
                std::vector<char> payload(8+text.size()+1);
                const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
                memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
                TCNimString rewritten{uint64_t(text.size()),payload.data()};originalLine(&rewritten,context);
                ++emission->rewritten;emission->currentValid=false;return;
            }
                if(role.index<instance.inlineExpressions.size()) {
                    if(!dependencyOperandsReady(instance,role.index,false)) {
                        instance.failed=true;
                        note("inline output is missing one of its declared operands: "+text);
                        originalLine(value,context);
                        return;
                    }
                    replacement=instantiateInlinePrefixOnce(instance,role.token,role.index,
                                                             refresh)+
                                text.substr(0,begin)+pinType+" ("+
                                instantiateInline(instance,role.token,role.index,false)+")";
                } else if(role.first) {
                    if(!instance.generated)instance.operands=operandTuple(text,begin,refresh);
                if(!dependencyOperandsReady(instance,role.index,false)) {
                    if(instance.warned.insert(role.token).second)
                        note("instance token "+std::to_string(role.token)+" exposes "+
                             std::to_string(instance.operands.size())+" operand(s) but the definition declares "+
                             std::to_string(instance.inputs)+" input(s); internal circuit kept: "+text);
                    instance.failed=true;
                    originalLine(value,context);
                    return;
                }
                const auto invocation=invocationText(instance,role.token,refresh,role.index);
                if(outWidth<=1) {
                    // Verified shape for one-bit pins.
                    replacement=invocation.prefix+text.substr(0,begin)+type+" "+invocation.expression;
                } else {
                    // Word pins: the call runs the callback as a discarded U1
                    // statement, then the value is assembled from its bits.
                    replacement=invocation.prefix+"var tc_io"+std::to_string(role.token)+
                                (instance.preciseDependencies?"o"+std::to_string(role.index):std::string())+
                                " = U1 "+invocation.expression+"\n"+
                                wordAssembly(pinType,role.token,role.index,outWidth,text.substr(0,begin));
                }
            } else {
                // The remaining output pins read the values the callback
                // produced during this cycle.
                if(outWidth<=1) {
                    replacement=text.substr(0,begin)+type+" game_engine.'tc_logic_out'(U64 "+
                        std::to_string(role.token)+", U64 "+std::to_string(role.index)+")";
                } else {
                    replacement=wordAssembly(pinType,role.token,role.index,outWidth,text.substr(0,begin));
                }
            }
            text=replacement;
            std::vector<char> payload(8+text.size()+1);const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
            TCNimString rewritten{uint64_t(text.size()),payload.data()};originalLine(&rewritten,context);
            ++emission->rewritten;emission->currentValid=false;return;
        }
    }
    originalLine(value,context);
}
inline uint64_t bridgeCycle(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi){
    return invoke(token,int64_t(cycle),packedLo,packedHi,TC_LOGIC_CYCLE);}
inline uint64_t bridgeTick(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi){
    return invokeTick(token,int64_t(cycle),packedLo,packedHi);}
inline uint64_t bridgePeek(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi){
    return invoke(token,int64_t(cycle),packedLo,packedHi,TC_LOGIC_REFRESH);}
inline uint64_t bridgeOutput(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi){
    return invokeOutput(token,selector,packedLo,packedHi);}
/* Historical six-argument diagnostics: same work as the direct pair, plus a
   check that the two extra arguments arrived intact.  New code never emits
   these calls; wide payloads use bridgeInputStage/bridgeDriveStage below. */
inline uint64_t wideMixLo(uint64_t cycle) { return cycle*0x9E3779B1ULL; }
inline uint64_t wideMixHi(uint64_t cycle) { return cycle*0x85EBCA6BULL+0x165667B1ULL; }
inline void checkWideCall(uint64_t cycle,uint64_t mixLo,uint64_t mixHi) {
    static std::atomic<uint64_t> calls{0},mismatches{0};
    const uint64_t count=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(mixLo!=wideMixLo(cycle)||mixHi!=wideMixHi(cycle)) {
        if(mismatches.fetch_add(1,std::memory_order_relaxed)<3&&logger)
            logger("Native logic: wide call mismatch cycle="+std::to_string(cycle)+
                   " got="+hex64(mixLo)+","+hex64(mixHi)+
                   " expected="+hex64(wideMixLo(cycle))+","+hex64(wideMixHi(cycle)));
    } else if(count%256==0&&logger) {
        logger("Native logic: wide call check calls="+std::to_string(count)+
               " mismatches="+std::to_string(mismatches.load(std::memory_order_relaxed)));
    }
}
inline uint64_t bridgeCycle6(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi,
                             uint64_t mixLo,uint64_t mixHi) {
    checkWideCall(cycle,mixLo,mixHi);
    return invoke(token,int64_t(cycle),packedLo,packedHi,TC_LOGIC_CYCLE);
}
inline uint64_t bridgePeek6(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi,
                            uint64_t mixLo,uint64_t mixHi) {
    checkWideCall(cycle,mixLo,mixHi);
    return invoke(token,int64_t(cycle),packedLo,packedHi,TC_LOGIC_REFRESH);
}
inline uint64_t bridgeOut(uint64_t token,uint64_t index){return readOutput(token,index);}
inline uint64_t bridgeBit(uint64_t token,uint64_t index){return readOutputBit(token,index);}
inline uint64_t bridgeInputDrive(uint64_t token,uint64_t packedLo,uint64_t packedHi){
    return latchInputDrive(token,packedLo,packedHi);
}
inline uint64_t bridgeInputStage(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi){
    return stageInput(token,selector,packedLo,packedHi,false);
}
inline uint64_t bridgeDriveStage(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi){
    return stageInput(token,selector,packedLo,packedHi,true);
}
inline uint64_t bridgeCycleStaged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t){
    return invokeStaged(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_CYCLE);
}
inline uint64_t bridgeTickStaged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t){
    return invokeStaged(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_CYCLE,
                        kTickOnlySelector);
}
inline uint64_t bridgePeekStaged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t){
    return invokeStaged(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_REFRESH);
}
inline uint64_t bridgeOutputStaged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t output){
    return invokeStagedOutput(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_CYCLE,
                              static_cast<uint32_t>(output));
}
inline uint64_t bridgeOutputPeekStaged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t output){
    return invokeStagedOutput(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_REFRESH,
                              static_cast<uint32_t>(output));
}
inline uint64_t bridgeOutputDrive(uint64_t token,uint64_t index){
    return readOutputDrive(token,index);
}
inline uint64_t bridgeDynamicConstant(uint64_t component,uint64_t fallback){
    return getDynamicConstant(component,fallback);
}
/* Fused RV32I integer ALU used by TC-Verilog's exact rv32i_flat fast path.
   Returning uint32_t before widening reproduces the Verilog 32-bit truncation. */
inline uint64_t bridgeVerilogAlu32(uint64_t operation,uint64_t a,uint64_t b){
    const uint32_t left=static_cast<uint32_t>(a);
    const uint32_t right=static_cast<uint32_t>(b);
    const uint32_t shift=right&31u;
    switch(operation&15u){
    case 0:return static_cast<uint32_t>(left+right);
    case 1:return static_cast<uint32_t>(left-right);
    case 2:return static_cast<uint32_t>(left<<shift);
    case 3:return static_cast<uint32_t>(static_cast<int32_t>(left)<static_cast<int32_t>(right));
    case 4:return static_cast<uint32_t>(left<right);
    case 5:return left^right;
    case 6:return left>>shift;
    case 7:return static_cast<uint32_t>(static_cast<int32_t>(left)>>shift);
    case 8:return left|right;
    case 9:return left&right;
    default:return 0;
    }
}
inline uint64_t bridgeVerilogRv32(uint64_t selector,uint64_t pc,uint64_t instr,
                                 uint64_t rs1,uint64_t rs2,uint64_t memory){
    struct Cache {
        uint32_t pc=0,instr=0,rs1=0,rs2=0,memory=0;
        bool bigEndian=false,valid=false;
        tc::rv32_intrinsic::Outputs outputs{};
    };
    static thread_local Cache cache;
    const bool bigEndian=(selector&8u)!=0;
    const uint32_t p=static_cast<uint32_t>(pc),i=static_cast<uint32_t>(instr),
                   a=static_cast<uint32_t>(rs1),b=static_cast<uint32_t>(rs2),
                   m=static_cast<uint32_t>(memory);
    if(!cache.valid||cache.bigEndian!=bigEndian||cache.pc!=p||cache.instr!=i||
       cache.rs1!=a||cache.rs2!=b||cache.memory!=m){
        cache={p,i,a,b,m,bigEndian,true,tc::rv32_intrinsic::evaluate(bigEndian,p,i,a,b,m)};
    }
    switch(selector&7u){
    case 0:return tc::rv32_intrinsic::packedInstructionOutputs(cache.outputs);
    case 1:return uint64_t(cache.outputs.pcNext)|(uint64_t(cache.outputs.memoryByteEnable)<<32);
    case 2:return cache.outputs.memoryAddress;
    case 3:return cache.outputs.regWriteData;
    case 4:return cache.outputs.memoryWriteData;
    default:return 0;
    }
}
inline void bridgeReset(){reset();}
inline void* compilerTable=nullptr;
inline void (*compilerInit)(void*)=nullptr;
inline void (*compilerSet)(void*,const TCNimString*,void*)=nullptr;
inline void (*compilerString)(void*,int64_t)=nullptr;
inline bool compilerReady=false;
/* The per-cycle scope tick (see src/scope_capture.hpp).  The loader inserts a
   call to it before every `cycle += 1` in the generated program, so a capture
   sees every cycle no matter who drives the run.  It has to be cheap when no
   capture is armed: the store's first statement is one relaxed atomic load.
   Declared before prepareCompiler because that is where it is registered. */
extern "C" __declspec(dllexport) inline void tc_scope_tick(uint64_t cycle) {
    tc::scope_capture::store().tick(cycle);
}
inline void prepareCompiler() {
    if(compilerReady)return;
    if(!read64(compilerTable,0x10))compilerInit(compilerTable);
    const char* names[]={"tc_logic_invoke","tc_logic_tick","tc_logic_peek","tc_logic_out","tc_logic_bit","tc_logic_reset",
                         "tc_dynamic_constant","tc_scope_tick","tc_verilog_alu32","tc_verilog_rv32","tc_logic_invoke6","tc_logic_peek6",
                         "tc_logic_drive_in","tc_logic_drive_out","tc_logic_stage",
                         "tc_logic_drive_stage","tc_logic_invoke_staged","tc_logic_tick_staged","tc_logic_peek_staged",
                         "tc_logic_output","tc_logic_output_staged","tc_logic_output_peek_staged"};
    void* pointers[]={reinterpret_cast<void*>(&bridgeCycle),reinterpret_cast<void*>(&bridgeTick),reinterpret_cast<void*>(&bridgePeek),
        reinterpret_cast<void*>(&bridgeOut),reinterpret_cast<void*>(&bridgeBit),reinterpret_cast<void*>(&bridgeReset),
        reinterpret_cast<void*>(&bridgeDynamicConstant),reinterpret_cast<void*>(&tc_scope_tick),
        reinterpret_cast<void*>(&bridgeVerilogAlu32),
        reinterpret_cast<void*>(&bridgeVerilogRv32),
        reinterpret_cast<void*>(&bridgeCycle6),reinterpret_cast<void*>(&bridgePeek6),
        reinterpret_cast<void*>(&bridgeInputDrive),reinterpret_cast<void*>(&bridgeOutputDrive),
        reinterpret_cast<void*>(&bridgeInputStage),reinterpret_cast<void*>(&bridgeDriveStage),
        reinterpret_cast<void*>(&bridgeCycleStaged),reinterpret_cast<void*>(&bridgeTickStaged),reinterpret_cast<void*>(&bridgePeekStaged),
        reinterpret_cast<void*>(&bridgeOutput),reinterpret_cast<void*>(&bridgeOutputStaged),
        reinterpret_cast<void*>(&bridgeOutputPeekStaged)};
      for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){TCNimString key{};size_t n=strlen(names[i]);compilerString(&key,n);key.length=n;
        memcpy(static_cast<char*>(key.data)+8,names[i],n+1);compilerSet(compilerTable,&key,pointers[i]);}
    compilerReady=true;
}
inline void compile(void* a,void* b,void* c) {
    auto* source=reinterpret_cast<TCNimString*>(static_cast<char*>(c)+8);
    if(!source->data || source->length>16000000){originalCompile(a,b,c);return;}
    std::string text(static_cast<const char*>(source->data)+8,source->length);
    /* The built-in Keyboard's emitted behaviour is corrected here, before
       anything reads the program (src/keyboard_fix.hpp).  The raw capture is
       kept for the dumps and for the tests that assert on what the game itself
       emitted; only the compiled text carries the fix. */
    const std::string emitted=text;
    const int keyboardFixed=tc::keyboard_fix::apply(text);
    const bool bridge=text.find("game_engine.'tc_")!=std::string::npos;
    if(dumpSourceEnabled()) {
        // Development dumps, off unless TC_MODLOADER_DUMP_SOURCE=1: one numbered
        // file per compile (a level compiles its level-IO program and the
        // schematic program separately), plus the stable name for the program
        // that actually carries a bridge call.  Left on, a long session fills
        // the game directory - the live install had 60+ files before this was
        // gated - and the playtests that assert on the generated source set the
        // variable themselves.
        static std::atomic<uint64_t> dumpCounter{0};
        const uint64_t number=dumpCounter.fetch_add(1);
        std::ofstream numbered("native-logic-source-"+std::to_string(number)+".txt");
        numbered<<emitted;
        // A second file only when the Keyboard rewrite changed something: the
        // numbered dump stays exactly what the game emitted, so existing
        // assertions on emission forms keep their meaning, and this one is the
        // program the JIT actually ran.
        if(keyboardFixed>0) {
            std::ofstream fixed("native-logic-source-keyboard-"+std::to_string(number)+".txt");
            fixed<<text;
        }
        if(text.find("game_engine.'tc_")!=std::string::npos ||
           text.find("// runtime constant ")!=std::string::npos) {
            std::ofstream debug("native-logic-source.txt");debug<<text;
        }
    }
    /* The per-cycle tick for the scope capture: one call before every
       `cycle += 1`, which is the point where that cycle's state is complete.
       A program with no such step is left exactly as it was. */
    const int ticks=tc::scope_capture::store().inject(text);
    if(!bridge&&ticks==0&&keyboardFixed==0){originalCompile(a,b,c);return;}
    prepareCompiler();
    if(text.rfind("extern windows_x64 game_engine",0)!=0)
        text="extern windows_x64 game_engine\n"+text;
    if(bridge){
        const std::string marker="def reset_sim() None {";
        auto pos=text.find(marker);
        if(pos!=std::string::npos)text.insert(pos+marker.size(),"\n    game_engine.'tc_logic_reset'()\n");
    }
    if(ticks>0)tc::scope_capture::store().noteInjected(ticks);
    std::vector<char> payload(8+text.size()+1);const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
    const auto saved=*source;*source={uint64_t(text.size()),payload.data()};
    originalCompile(a,b,c);*source=saved;
}
inline void jit(void* sequence,int64_t entry) {
    static const bool enabled=[] {const char* value=std::getenv("TC_MODLOADER_JIT_OPT");return !(value && value[0]=='0');}();
    auto* source=static_cast<TCNimString*>(sequence);
    if(!enabled || !source || !source->data || source->length>16000000) {originalJit(sequence,entry);return;}
    tc::simulation_jit::Result result;
    std::vector<char> payload;TCNimString rewritten{};
    try {
        result=tc::simulation_jit::optimize(static_cast<const uint8_t*>(source->data)+8,size_t(source->length),entry);
        if(!result.code.empty()) {
            payload.resize(8+result.code.size());
            const uint64_t capacity=uint64_t(result.code.size())|(uint64_t(1)<<62);
            memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,result.code.data(),result.code.size());
            rewritten={uint64_t(result.code.size()),payload.data()};
            if(logger)logger("Native simulation JIT: removed "+std::to_string(result.removed)+" redundant instructions, bytes "+
                std::to_string(source->length)+" -> "+std::to_string(result.code.size())+", entry "+std::to_string(entry)+" -> "+std::to_string(result.entry)+
                ", constant shifts "+std::to_string(result.specialized)+", narrow masks "+std::to_string(result.masked)+
                ", dead constants "+std::to_string(result.deadConstants)+", dead copies "+std::to_string(result.deadCopies)+", redundant masks "+std::to_string(result.redundantMasks)+
                ", fallthrough jumps "+std::to_string(result.fallthroughJumps)+", stack forwards "+std::to_string(result.stackForwards)+
                ", equivalent copies "+std::to_string(result.equivalentCopies)+", signed extensions "+std::to_string(result.signedExtensions)+
                ", unused extensions "+std::to_string(result.unusedExtensions)+", unused masks "+std::to_string(result.unusedMasks)+
                ", flow copies "+std::to_string(result.flowCopies)+", redundant stores "+std::to_string(result.redundantStores));
            if(dumpSourceEnabled()) {
                static std::atomic<uint64_t> serial{0};const auto number=serial.fetch_add(1);
                std::ofstream file("native-jit-optimized-"+std::to_string(number)+".bin",std::ios::binary);
                file.write(reinterpret_cast<const char*>(result.code.data()),std::streamsize(result.code.size()));
            }
        }
    } catch(...) {originalJit(sequence,entry);return;}
    if(result.code.empty()) {originalJit(sequence,entry);return;}
    // The game copies this byte sequence into executable memory before it
    // runs it. Keep the private buffer alive until that execution returns.
    originalJit(&rewritten,result.entry);
}
inline bool install(const std::map<std::string,void*>& symbols,const std::map<std::string,void*>& compiler,std::set<void*>& owned,std::function<void(const std::string&)> log) {
    auto get=[&](const char* name)->void*{auto it=symbols.find(name);return it==symbols.end()?nullptr:it->second;};
    auto cg=[&](const char* name)->void*{auto it=compiler.find(name);return it==compiler.end()?nullptr:it->second;};
    compilerTable=cg("foreign_lib_pointers__OOZtypes_u32223");
    compilerInit=reinterpret_cast<void(*)(void*)>(cg("get_lib_pointers__OOZpassesZjitZjit_u7"));
    compilerSet=reinterpret_cast<void(*)(void*,const TCNimString*,void*)>(cg("X5BX5Deq___OOZpassesZjitZjit_u294"));
    compilerString=reinterpret_cast<void(*)(void*,int64_t)>(cg("rawNewString"));
    if(!compilerTable||!compilerInit||!compilerSet||!compilerString)return false;
    getPrototype=reinterpret_cast<void(*)(uint64_t,void*)>(get("get_custom_prototype__modelZboardZcustom95prototype95list_u451"));
    destroyPrototype=reinterpret_cast<void(*)(void*)>(get("eqdestroy___modelZboardZprototype95list_u3259"));
    stateBuffer=reinterpret_cast<unsigned char**>(get("simulation_state__modelZsimulator95types_u81"));
    void* targets[]={get("add_circuit_code__modelZsimulationZcode95gen_u4263"),get("add_line__modelZsimulationZcode95gen_u2129"),get("handle_request_compile_and_run__modelZsimulationZsimulator95functions_u20")};
    void* detours[]={reinterpret_cast<void*>(&circuit),reinterpret_cast<void*>(&line),reinterpret_cast<void*>(&compile)};
    void** originals[]={reinterpret_cast<void**>(&originalCircuit),reinterpret_cast<void**>(&originalLine),reinterpret_cast<void**>(&originalCompile)};
    if(!getPrototype||!destroyPrototype)return false;
    size_t created=0;
    for(;created<3;++created)if(!targets[created]||MH_CreateHook(targets[created],detours[created],originals[created])!=MH_OK)break;
    bool ok=created==3;
    if(ok)for(auto target:targets)if(MH_EnableHook(target)!=MH_OK)ok=false;
    if(!ok){for(size_t i=0;i<created;++i){MH_DisableHook(targets[i]);MH_RemoveHook(targets[i]);}return false;}
    for(auto target:targets)owned.insert(target);logger=std::move(log);installed=true;
    auto jitTarget=get("jit__modelZsimulationZjitZjit_u1807");
    if(jitTarget && MH_CreateHook(jitTarget,reinterpret_cast<void*>(&jit),reinterpret_cast<void**>(&originalJit))==MH_OK) {
        if(MH_EnableHook(jitTarget)==MH_OK)owned.insert(jitTarget);
        else {MH_RemoveHook(jitTarget);if(logger)logger("Native simulation JIT: optimization hook unavailable");}
    } else if(logger)logger("Native simulation JIT: optimization hook unavailable");
    return true;
}
}

extern "C" __declspec(dllexport) inline uint64_t tc_logic_invoke(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invoke(token,static_cast<int64_t>(cycle),packedLo,packedHi,TC_LOGIC_CYCLE);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_tick(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invokeTick(token,static_cast<int64_t>(cycle),packedLo,packedHi);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_peek(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invoke(token,static_cast<int64_t>(cycle),packedLo,packedHi,TC_LOGIC_REFRESH);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_output(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invokeOutput(token,selector,packedLo,packedHi);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_stage(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::stageInput(token,selector,packedLo,packedHi,false);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_drive_stage(uint64_t token,uint64_t selector,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::stageInput(token,selector,packedLo,packedHi,true);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_invoke_staged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t reserved) {
    (void)reserved;
    return tc::logic::invokeStaged(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_CYCLE);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_tick_staged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t reserved) {
    (void)reserved;
    return tc::logic::invokeStaged(token,static_cast<int64_t>(cycle),chunks,
                                   TC_LOGIC_CYCLE,tc::logic::kTickOnlySelector);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_peek_staged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t reserved) {
    (void)reserved;
    return tc::logic::invokeStaged(token,static_cast<int64_t>(cycle),chunks,TC_LOGIC_REFRESH);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_output_staged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t output) {
    return tc::logic::invokeStagedOutput(token,static_cast<int64_t>(cycle),chunks,
                                         TC_LOGIC_CYCLE,static_cast<uint32_t>(output));
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_output_peek_staged(uint64_t token,uint64_t cycle,uint64_t chunks,uint64_t output) {
    return tc::logic::invokeStagedOutput(token,static_cast<int64_t>(cycle),chunks,
                                         TC_LOGIC_REFRESH,static_cast<uint32_t>(output));
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_out(uint64_t token,uint64_t index) {
    return tc::logic::readOutput(token,index);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_bit(uint64_t token,uint64_t index) {
    return tc::logic::readOutputBit(token,index);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_reset() {tc::logic::reset();return 0;}
extern "C" __declspec(dllexport) inline void tc_dynamic_constant_set(uint64_t component,uint64_t value) {
    tc::logic::setDynamicConstant(component,value);
}
