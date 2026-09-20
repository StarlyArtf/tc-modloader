#include <map>
#include <set>
#include <functional>
#include <fstream>
#include <cassert>
#include <iostream>
#include "../vendor/minhook/include/MinHook.h"
#include "../src/component_definition.hpp"
#include "../src/native_logic.hpp"
#include "../sdk/tc_native_component.h"

static void noop(TCLogicIO*) {}
static unsigned registrations;
static int registered(void*, const TCNativeComponentDefinition* d) {
    ++registrations;
    assert(tc::component_definition::valid(d));
    return 0;
}
static std::vector<std::string> lines;
static void output(const tc::TCNimString* s, void*) {
    lines.emplace_back(static_cast<const char*>(s->data)+8,s->length);
}
static void emit(const std::string& s) {
    std::vector<char> data(8+s.size());
    memcpy(data.data()+8,s.data(),s.size());
    tc::TCNimString value{s.size(),data.data()};
    tc::logic::line(&value,nullptr);
}
int main() {
    tc::TCNativeComponent c;
    c.id=0xf000000000000001ULL; c.name="Declarative";
    c.inputs={{"carry",1},{"A",8},{"B",8}};
    c.outputs={{"sum",8},{"carry",1}}; c.callback=noop;
    TCHost host{};host.size=sizeof(host);host.api_version=TC_MOD_API_VERSION;
    host.register_component=registered;
    assert(c.registerWith(&host)==0 && registrations==1);
    host.size=offsetof(TCHost,register_component);
    assert(c.registerWith(&host)==-1 && registrations==1);
    assert(c.registerWith(nullptr)==-1);
    TCComponentPin pins[8];for(auto& p:pins)p={"pin",16};
    TCNativeComponentDefinition d{};d.size=sizeof(d);d.custom_id=c.id;d.name="test";
    d.inputs=pins;d.outputs=pins;d.input_count=8;d.output_count=8;d.callback=noop;
    assert(tc::component_definition::valid(&d));
    const auto bytes=tc::component_definition::encode(d);
    assert(!bytes.empty() && bytes[0]==14);
    std::ofstream file("build/declarative-8x8.data",std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();
    pins[0].bits=17;assert(!tc::component_definition::valid(&d));pins[0].bits=16;
    for(auto bad:{0u,65u}) {pins[0].bits=bad;assert(!tc::component_definition::valid(&d));}pins[0].bits=16;
    d.input_count=0;assert(!tc::component_definition::valid(&d));d.input_count=8;
    d.output_count=9;assert(!tc::component_definition::valid(&d));d.output_count=8;
    d.custom_id=0;assert(!tc::component_definition::valid(&d));d.custom_id=c.id;
    d.callback=nullptr;assert(!tc::component_definition::valid(&d));d.callback=noop;
    d.gate_cost=UINT64_MAX;assert(!tc::component_definition::valid(&d));d.gate_cost=1;

    // A 64-bit pin crossing payload words used to overrun the payload array.
    // Construct payload independently, one bit at a time, then verify decoding.
    using namespace tc::logic;
    auto definition=std::make_shared<Definition>();
    definition->inputs=3;definition->outputs=1;definition->inputWidths={1,64,63};
    placeInputs(definition->inputWidths,3,definition->inputWord,definition->inputShift);
    definition->api.callback=[](TCLogicIO* io) {
        assert(io->inputs[0]==1);
        assert(io->inputs[1]==0xfedcba9876543210ULL);
        assert(io->inputs[2]==0x7123456789abcdefULL);
        io->outputs[0]=1;
    };
    auto binding=std::make_shared<Binding>();binding->definition=definition;bindings.push_back(binding);
    const uint64_t values[]={1,0xfedcba9876543210ULL,0x7123456789abcdefULL};
    uint64_t packed[2]={};unsigned cursor=0;
    for(unsigned i=0;i<3;++i)for(unsigned b=0;b<definition->inputWidths[i];++b,++cursor)
        if((values[i]>>b)&1)packed[cursor/64]|=uint64_t(1)<<(cursor%64);
    assert(invoke(1,0,packed[0],packed[1],TC_LOGIC_CYCLE)==1);

    // Nontrivial collector emission order must not reorder callback inputs.
    originalLine=output;
    for(bool refresh:{false,true}) {
        Emission e;e.instances[1].generated=true;
        auto& i=e.instances[1];i.inputs=3;i.outputs=1;i.widths={1,64,63};i.outWidths={1};i.operands.resize(3);
        e.nodes[10]={1,0,false,true};e.nodes[11]={1,1,false,true};e.nodes[12]={1,2,false,true};e.nodes[15]={1,0,true,false};
        emission=&e;lines.clear();
        for(auto k:{2,0,1}) {
            emit("// "+std::to_string(10+k));
            emit(refresh?"let value_id"+std::to_string(10+k)+" = ~(load(<U64>, #STATE + "+std::to_string(100+k)+"))":
                "var vid"+std::to_string(10+k)+" = U64 ~(U64 vid"+std::to_string(100+k)+")");
        }
        emit("// 15");emit(refresh?"let value_id15 = ~(load(<U1>, #STATE + 14))":"var vid15 = U1 ~(U1 vid14)");
        assert(e.rewritten==1 && !i.failed);
        assert(lines.back().find(refresh?"tc_logic_peek":"tc_logic_invoke")!=std::string::npos);
        assert(i.operands[0].find("100")!=std::string::npos && i.operands[1].find("101")!=std::string::npos);
        assert(lines.back().find(">> 63")!=std::string::npos);
        emission=nullptr;
    }
    std::cout<<"PASS declarative API guards, 8x8 definition, invalid shapes, cross-word payloads, collector ordering and both emission phases\n";
}
