#pragma once
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_game_model.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace tc::logic {
// The generated invoke call keeps the verified four-argument shape
// (token, cycle, payload lo, payload hi), so up to 128 input bits fit.
inline constexpr uint32_t kMaxBridgeInputs=8;
inline constexpr uint32_t kMaxBridgeOutputs=8;
inline constexpr uint32_t kMaxPinBits=64;
inline constexpr uint32_t kMaxInputBits=128;
struct Definition {
    TCLogicDefinition api;
    uint32_t inputs=0,outputs=0;
    std::array<uint32_t,kMaxBridgeInputs> inputWidths{};
    std::array<uint32_t,kMaxBridgeOutputs> outputWidths{};
    // Payload word index and bit offset of each input inside the invoke call.
    std::array<uint32_t,kMaxBridgeInputs> inputWord{};
    std::array<uint32_t,kMaxBridgeInputs> inputShift{};
    uint32_t inputBits=0;
    bool wideOutputs=false;
    bool active=false;
};
// Pack contiguously, including pins crossing the two-word boundary.
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
    std::shared_ptr<Definition> definition;
    uint64_t instance=0;
    std::array<uint64_t,8> state{};
    std::array<uint64_t,8> outputs{};
    std::mutex mutex;
};
inline std::mutex registryMutex;
inline std::unordered_map<uint64_t,std::shared_ptr<Definition>> definitions;
inline std::vector<std::shared_ptr<Binding>> bindings;
inline std::map<std::pair<uint64_t,uint64_t>,uint64_t> bindingKeys;
inline std::function<void(const std::string&)> logger;
inline bool installed=false;
inline void (*getPrototype)(uint64_t,void*)=nullptr;
inline void (*destroyPrototype)(void*)=nullptr;
// Simulation state buffer: foreign call results cannot be consumed as words, so
// word outputs are published here as one byte per bit and the generated program
// loads them back the same way the game's own word components do.
inline unsigned char** stateBuffer=nullptr;
inline constexpr uint64_t kBridgeStateBase=0x9a0000;
inline constexpr uint64_t kBridgeStateStride=512;
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
// Shape validation.  Only 2 x 1-bit in, 1 x 1-bit out is bridged; the game
// itself keeps generating code for every other part of the board.
inline std::string describePrototype(uint64_t custom_id,const TCPrototype& p) {
    const uint64_t inputs=prototypeInputCount(p),outputs=prototypeOutputCount(p);
    std::string text="custom "+hex64(custom_id)+" inputs="+std::to_string(inputs)+
        " outputs="+std::to_string(outputs);
    for(uint64_t i=0;i<inputs&&i<4;++i)text+=" in"+std::to_string(i)+"="+pinText(readPinGeometry(p,true,i));
    for(uint64_t i=0;i<outputs&&i<4;++i)text+=" out"+std::to_string(i)+"="+pinText(readPinGeometry(p,false,i));
    return text;
}
// Shape limits of the bridge: up to eight pins per direction, each 1..64 bits,
// with the total input width fitting the two payload words of the invoke call.
inline bool supportedScaffold(uint64_t custom_id,const TCPrototype& p,std::string& detail,
                              std::array<uint32_t,kMaxBridgeInputs>& inputWidths,
                              std::array<uint32_t,kMaxBridgeOutputs>& outputWidths,
                              uint32_t& inputBits) {
    detail=describePrototype(custom_id,p);
    const uint64_t inputs=prototypeInputCount(p),outputs=prototypeOutputCount(p);
    if(inputs<1||inputs>kMaxBridgeInputs||outputs<1||outputs>kMaxBridgeOutputs)return false;
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
inline int add(const TCLogicDefinition* d) {
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
    const bool valid=supportedScaffold(d->custom_id,p,detail,inputWidths,outputWidths,inputBits);
    const uint32_t inputs=valid?static_cast<uint32_t>(prototypeInputCount(p)):0;
    const uint32_t outputs=valid?static_cast<uint32_t>(prototypeOutputCount(p)):0;
    destroyPrototype(&p);
    if(!valid) {
        note("registration rejected: "+detail+
             " (supported shape is up to eight pins per direction, each 1..64 bits, total input width <= 128)");
        return -2;
    }
    std::lock_guard<std::mutex> lock(registryMutex);
    if(definitions.count(d->custom_id))return -3;
    auto entry=std::make_shared<Definition>();entry->api=*d;
    entry->inputs=inputs;entry->outputs=outputs;
    entry->inputWidths=inputWidths;entry->outputWidths=outputWidths;entry->inputBits=inputBits;
    for(uint32_t i=0;i<outputs;++i)if(outputWidths[i]>1)entry->wideOutputs=true;
    placeInputs(entry->inputWidths,inputs,entry->inputWord,entry->inputShift);
    definitions.emplace(d->custom_id,std::move(entry));
    note("registered "+detail+" shape="+std::to_string(inputs)+"in/"+std::to_string(outputs)+"out");
    return 0;
}
inline void finish(const std::vector<uint64_t>& ids,bool success) {
    std::lock_guard<std::mutex> lock(registryMutex);
    for(auto id:ids) {auto it=definitions.find(id);if(it==definitions.end())continue;
        if(success)it->second->active=true;else definitions.erase(it);}
}
// Runs the callback once for an instance and keeps its outputs for the
// tc_logic_out readback used by the remaining output drivers.  Input words
// arrive packed into two payload arguments because the generated code cannot
// pass more than four arguments through the JIT without tripping its register
// allocator; each pin is masked to its declared width.
inline uint64_t invoke(uint64_t token,int64_t cycle,uint64_t packedLo,uint64_t packedHi,uint32_t phase) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        std::lock_guard<std::mutex> lock(binding->mutex);
        const uint32_t inputCount=binding->definition->inputs;
        const uint32_t outputCount=binding->definition->outputs;
        const uint64_t payload[2]={packedLo,packedHi};
        TCLogicIO io{};io.size=sizeof(io);io.phase=phase;io.instance_id=binding->instance;io.cycle=cycle;
        io.input_count=inputCount;io.output_count=outputCount;io.user=binding->definition->api.user;
        for(uint32_t i=0;i<inputCount&&i<8;++i) {
            const uint32_t bits=binding->definition->inputWidths[i];
            const uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);
            const auto word=binding->definition->inputWord[i],shift=binding->definition->inputShift[i];
            uint64_t value=payload[word]>>shift;
            if(shift+bits>64)value|=payload[word+1]<<(64-shift);
            io.inputs[i]=value&mask;
        }
        std::copy(binding->state.begin(),binding->state.end(),io.state);
        binding->definition->api.callback(&io);
        if(phase==TC_LOGIC_CYCLE)std::copy(std::begin(io.state),std::end(io.state),binding->state.begin());
        for(uint32_t i=0;i<8;++i)binding->outputs[i]=io.outputs[i]&1;
        if(binding->definition->wideOutputs)publishWordOutputs(token,io);
        return binding->outputs[0];
    }catch(...) {if(logger)logger("Native logic: callback threw; outputs forced to zero");return 0;}
}
inline uint64_t readOutput(uint64_t token,uint64_t index) noexcept {
    try {
        std::shared_ptr<Binding> binding;
        {std::lock_guard<std::mutex> lock(registryMutex);if(!token||token>bindings.size())return 0;binding=bindings[token-1];}
        std::lock_guard<std::mutex> lock(binding->mutex);
        return index<8?binding->outputs[index]&1:0;
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
        if(pin>=8)return 0;
        return (binding->outputs[pin]>>bit)&1;
    }catch(...) {return 0;}
}
inline void reset() noexcept {
    try {
        std::vector<std::shared_ptr<Binding>> snapshot;
        {std::lock_guard<std::mutex> lock(registryMutex);snapshot=bindings;}
        for(auto& binding:snapshot){std::lock_guard<std::mutex> lock(binding->mutex);
            binding->state.fill(0);binding->outputs.fill(0);
            TCLogicIO io{};io.size=sizeof(io);io.phase=TC_LOGIC_RESET;
            io.instance_id=binding->instance;io.cycle=-1;io.user=binding->definition->api.user;
            io.input_count=binding->definition->inputs;io.output_count=binding->definition->outputs;
            binding->definition->api.callback(&io);
            std::copy(std::begin(io.state),std::end(io.state),binding->state.begin());}
        if(!snapshot.empty())note("reset "+std::to_string(snapshot.size())+" instance(s)");
    }catch(...) {if(logger)logger("Native logic: reset callback threw");}
}
using AddCircuit=void(*)(void*,void*,uint8_t,void*,void*,void*);
using AddLine=void(*)(const TCNimString*,void*);
using CompileRun=void(*)(void*,void*,void*);
inline AddCircuit originalCircuit=nullptr;
inline AddLine originalLine=nullptr;
inline CompileRun originalCompile=nullptr;
struct Emission {
    // One entry per node that will be replaced, keyed by its node index.
    struct Role {uint64_t token=0;uint32_t index=0;bool first=false;bool collector=false;};
    // Per compiled instance: the shape the definition declares.
    struct Instance {
        uint32_t inputs=0,outputs=0;
        std::array<uint32_t,kMaxBridgeInputs> widths{};
        std::array<uint32_t,kMaxBridgeOutputs> outWidths{};
        std::vector<std::string> operands;
        bool generated=false;
        bool ready=false,failed=false;
        std::set<uint64_t> warned;
    };
    std::unordered_map<uint64_t,Role> nodes;
    std::map<uint64_t,Instance> instances;
    Role current{};
    bool currentValid=false;
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
    return (kind>=0x03&&kind<=0x0b)||(kind>=0x12&&kind<=0x1e)||kind==0x2a;
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

inline void circuit(void* a,void* b,uint8_t c,void* d,void* e,void* f) {
    Emission context;
    context.mode=c;
    // The code generator receives the flattened component sequence as arg 2.
    const auto count=read64(b,0),data=read64(b,8);
    if(count<=1000000 && data) {
        auto node=[&](size_t i){return reinterpret_cast<const unsigned char*>(data+8+i*0x238);};
        std::unordered_map<uint64_t,size_t> parents;
        for(size_t i=0;i<count;++i)if(node(i)[0]==0x4e)parents.emplace(read64(node(i),8),i);
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
            const bool generated=def->second->api.version==3;
            const size_t prefix=generated?2*def->second->inputs-1:0;
            if(nodesOfInstance.size()!=prefix+def->second->outputs) {
                note("instance "+hex64(instance)+" of custom "+hex64(custom)+" has "+
                     std::to_string(nodesOfInstance.size())+" internal logic node(s) for "+
                     std::to_string(def->second->outputs)+" output(s); internal circuit kept");
                continue;
            }
            auto key=std::make_pair(custom,instance);
            auto found=bindingKeys.find(key);
            uint64_t token;
            if(found!=bindingKeys.end())token=found->second;
            else {auto binding=std::make_shared<Binding>();binding->definition=def->second;binding->instance=instance;
                bindings.push_back(binding);token=bindings.size();bindingKeys.emplace(key,token);
                note("bound instance "+hex64(instance)+" of custom "+hex64(custom)+" as token "+std::to_string(token));}
            std::sort(nodesOfInstance.begin(),nodesOfInstance.end());
            for(size_t k=0;k<nodesOfInstance.size();++k) {
                if(generated && k<def->second->inputs)
                    context.nodes.emplace(nodesOfInstance[k],Emission::Role{token,static_cast<uint32_t>(k),false,true});
                else if(k>=prefix)
                    context.nodes.emplace(nodesOfInstance[k],Emission::Role{token,static_cast<uint32_t>(k-prefix),k==prefix,false});
            }
            auto& state=context.instances[token];
            state.generated=generated;
            if(generated)state.operands.resize(def->second->inputs);
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
    auto* previous=emission;emission=&context;
    originalCircuit(a,b,c,d,e,f);
    emission=previous;
    if(context.rewritten && logger)logger("Native logic: emitted "+std::to_string(context.rewritten)+" callback(s), mode="+std::to_string(c));
}
// Word-width result of an output pin.  Foreign call results survive the JIT only
// as single bits, so the callback publishes its output bits into reserved
// simulation-state slots and the generated program loads and assembles them,
// mirroring the way the game's own word components read their inputs.
inline std::string wordAssembly(const std::string& type,uint64_t token,uint32_t pin,
                               uint32_t width,const std::string& prefix) {
    std::string bits;
    for(uint32_t bit=0;bit<width&&bit<64;++bit) {
        std::string term="((("+type+" (load(<U1>, #SIMULATION_STATE + "+
                         std::to_string(stateSlot(token,pin,bit))+")))) & 1)";
        if(bit)term="("+term+" << "+std::to_string(bit)+")";
        if(bit)bits+=" | ";
        bits+=term;
    }
    return prefix+type+" ("+bits+")";
}

// Borrowed Nim string payload: the callee only reads/copies this line.
inline void line(const TCNimString* value,void* context) {
    if(!emission || !value || !value->data || value->length>1000000){originalLine(value,context);return;}
    std::string text(static_cast<const char*>(value->data)+8,value->length);
    auto comment=text.find("// ");
    if(comment!=std::string::npos) {
        std::istringstream in(text.substr(comment+3));uint64_t index;
        emission->currentValid=false;
        if(in>>index){auto it=emission->nodes.find(index);
            if(it!=emission->nodes.end()){emission->current=it->second;emission->currentValid=true;}}
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
                } else instance.operands[role.index]=operands[0];
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
            if(role.first) {
                if(!instance.generated)instance.operands=operandTuple(text,begin,refresh);
                std::ostringstream call;
                if(instance.operands.size()!=instance.inputs ||
                   std::any_of(instance.operands.begin(),instance.operands.end(),[](const auto& v){return v.empty();})) {
                    if(instance.warned.insert(role.token).second)
                        note("instance token "+std::to_string(role.token)+" exposes "+
                             std::to_string(instance.operands.size())+" operand(s) but the definition declares "+
                             std::to_string(instance.inputs)+" input(s); internal circuit kept: "+text);
                    instance.failed=true;
                    originalLine(value,context);
                    return;
                }
                // Pack the operands into two payload words: token, cycle,
                // payload lo, payload hi.  Four arguments are the widest call
                // shape the JIT register allocator handles for a foreign
                // function; each operand is shifted into its assigned word.
                std::array<std::string,2> payload;
                std::array<uint32_t,kMaxBridgeInputs> word{},shift{};
                placeInputs(instance.widths,static_cast<uint32_t>(instance.operands.size()),word,shift);
                for(size_t i=0;i<instance.operands.size();++i) {
                    std::string term="U64 ("+instance.operands[i]+")";
                    if(shift[i])term+=" << "+std::to_string(shift[i]);
                    if(shift[i])term="("+term+")";
                    payload[word[i]]+= (payload[word[i]].empty()?"":" | ")+term;
                    if(shift[i]+instance.widths[i]>64) {
                        const std::string high="(U64 ("+instance.operands[i]+") >> "+std::to_string(64-shift[i])+")";
                        payload[word[i]+1]+=(payload[word[i]+1].empty()?"":" | ")+high;
                    }
                }
                call<<"game_engine.'"<<(refresh?"tc_logic_peek":"tc_logic_invoke")<<"'(U64 "<<role.token
                    <<", U64 (cycle"<<(refresh?"":" + 1")<<"), ";
                call<<(payload[0].empty()?"U64 0":payload[0])
                    <<", "<<(payload[1].empty()?"U64 0":payload[1])<<")";
                if(outWidth<=1) {
                    // Verified shape for one-bit pins.
                    replacement=text.substr(0,begin)+type+" "+call.str();
                } else {
                    // Word pins: the call runs the callback as a discarded U1
                    // statement, then the value is assembled from its bits.
                    replacement="var tc_io"+std::to_string(role.token)+" = U1 "+call.str()+"\n"+
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
inline uint64_t bridgePeek(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi){
    return invoke(token,int64_t(cycle),packedLo,packedHi,TC_LOGIC_REFRESH);}
inline uint64_t bridgeOut(uint64_t token,uint64_t index){return readOutput(token,index);}
inline uint64_t bridgeBit(uint64_t token,uint64_t index){return readOutputBit(token,index);}
inline void bridgeReset(){reset();}
inline void* compilerTable=nullptr;
inline void (*compilerInit)(void*)=nullptr;
inline void (*compilerSet)(void*,const TCNimString*,void*)=nullptr;
inline void (*compilerString)(void*,int64_t)=nullptr;
inline bool compilerReady=false;
inline void prepareCompiler() {
    if(compilerReady)return;
    if(!read64(compilerTable,0x10))compilerInit(compilerTable);
    const char* names[]={"tc_logic_invoke","tc_logic_peek","tc_logic_out","tc_logic_bit","tc_logic_reset"};
    void* pointers[]={reinterpret_cast<void*>(&bridgeCycle),reinterpret_cast<void*>(&bridgePeek),
        reinterpret_cast<void*>(&bridgeOut),reinterpret_cast<void*>(&bridgeBit),reinterpret_cast<void*>(&bridgeReset)};
    for(size_t i=0;i<5;++i){TCNimString key{};size_t n=strlen(names[i]);compilerString(&key,n);key.length=n;
        memcpy(static_cast<char*>(key.data)+8,names[i],n+1);compilerSet(compilerTable,&key,pointers[i]);}
    compilerReady=true;
}
inline void compile(void* a,void* b,void* c) {
    auto* source=reinterpret_cast<TCNimString*>(static_cast<char*>(c)+8);
    if(!source->data || source->length>16000000){originalCompile(a,b,c);return;}
    std::string text(static_cast<const char*>(source->data)+8,source->length);
    // Development dumps: one numbered file per compile (a level compiles its
    // level-IO program and the schematic program separately), plus the stable
    // name for the program that actually carries a bridge call.
    {
        static std::atomic<uint64_t> dumpCounter{0};
        std::ofstream numbered("native-logic-source-"+std::to_string(dumpCounter.fetch_add(1))+".txt");
        numbered<<text;
    }
    if(text.find("game_engine.'tc_logic_")!=std::string::npos) {
        std::ofstream debug("native-logic-source.txt");debug<<text;
    }
    if(text.find("game_engine.'tc_logic_")==std::string::npos){originalCompile(a,b,c);return;}
    prepareCompiler();
    text="extern windows_x64 game_engine\n"+text;
    const std::string marker="def reset_sim() None {";
    auto pos=text.find(marker);
    if(pos!=std::string::npos)text.insert(pos+marker.size(),"\n    game_engine.'tc_logic_reset'()\n");
    std::vector<char> payload(8+text.size()+1);const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);memcpy(payload.data(),&capacity,8);memcpy(payload.data()+8,text.data(),text.size());
    const auto saved=*source;*source={uint64_t(text.size()),payload.data()};
    originalCompile(a,b,c);*source=saved;
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
    for(auto target:targets)owned.insert(target);logger=std::move(log);installed=true;return true;
}
}

extern "C" __declspec(dllexport) inline uint64_t tc_logic_invoke(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invoke(token,static_cast<int64_t>(cycle),packedLo,packedHi,TC_LOGIC_CYCLE);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_peek(uint64_t token,uint64_t cycle,uint64_t packedLo,uint64_t packedHi) {
    return tc::logic::invoke(token,static_cast<int64_t>(cycle),packedLo,packedHi,TC_LOGIC_REFRESH);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_out(uint64_t token,uint64_t index) {
    return tc::logic::readOutput(token,index);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_bit(uint64_t token,uint64_t index) {
    return tc::logic::readOutputBit(token,index);
}
extern "C" __declspec(dllexport) inline uint64_t tc_logic_reset() {tc::logic::reset();return 0;}


