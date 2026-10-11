// Reuse the real example's isolated level runner; only replace its registration
// descriptor so the same level exercises all eight generated input/output pins.
#define tc_mod_load byte_adder_example_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load
#include <stdexcept>

static TCHost testHost;
static int (*realRegister)(void*,const TCNativeComponentDefinition*);
#ifdef TC_DECLARATIVE_V3
static TCComponentTypesApiV2 typesV3{};
static void stressV3(TCLogicIOV3* io) {
    if(io->phase==TC_LOGIC_CYCLE) {
        io->outputs[0].value=(io->inputs[0].value*2)&255;
        io->outputs[0].drive_mask=0xff;
        log("declarative: double input="+std::to_string(io->inputs[0].value)+
            " output="+std::to_string(io->outputs[0].value)+
            " drive="+std::to_string(io->inputs[0].drive_mask));
    }
    /* Exercise a real disconnected output on the UI refresh path while the
       cycle path stays driven so the level can still verify arithmetic. */
}
#endif
static void stress(TCLogicIO* io) {
#ifdef TC_DECLARATIVE_SINGLE
    if(io->phase!=TC_LOGIC_RESET) {
        io->outputs[0]=(io->inputs[0]*2)&255;
        if(io->phase==TC_LOGIC_CYCLE)
            log("declarative: double input="+std::to_string(io->inputs[0])+" output="+std::to_string(io->outputs[0]));
    }
#else
    addBytes(io);
    if(io->phase!=TC_LOGIC_RESET) {
        const bool order=io->inputs[3]==io->inputs[0] && io->inputs[4]==io->inputs[1] &&
            io->inputs[5]==io->inputs[0] && io->inputs[6]==io->inputs[1] && io->inputs[7]==io->inputs[2];
        const uint64_t total=io->inputs[5]+io->inputs[6]+io->inputs[7];
        io->outputs[6]=order ? total&255 : 255;
        io->outputs[7]=(total>>8)&1;
        if(io->phase==TC_LOGIC_CYCLE && io->cycle<3) {
            std::string values;
            for(unsigned i=0;i<8;++i)values+=std::to_string(io->inputs[i])+",";
            log("declarative: inputs="+values+" order="+std::to_string(order));
        }
    }
#endif
}
static int registerStress(void* context,const TCNativeComponentDefinition* source) {
    static const TCComponentPin ins[]={{"carry",1},{"A",8},{"B",8},{"carry copy",1},
        {"A copy",8},{"carry last",1},{"A last",8},{"B last",8}};
    static const TCComponentPin outs[]={{"unused0",8},{"unused1",1},{"unused2",8},{"unused3",1},
        {"unused4",8},{"unused5",1},{"sum",8},{"carry",1}};
    auto d=*source;d.inputs=ins;d.outputs=outs;d.input_count=8;d.output_count=8;d.callback=stress;
#ifdef TC_DECLARATIVE_SINGLE
    d.custom_id=0x44424C385F303031ULL;
    d.inputs=&ins[1];d.outputs=&outs[0];d.input_count=1;d.output_count=1;
#endif
#ifdef TC_DECLARATIVE_V3
    TCComponentPinV2 v3In{"in","A",d.inputs[0].bits,0};
    TCComponentPinV2 v3Out{"out","Y",d.outputs[0].bits,0};
    TCComponentTypeDefinitionV3 v3{};
    v3.size=sizeof(v3);v3.version=TC_COMPONENT_TYPES_VERSION_3;
    v3.custom_id=d.custom_id;v3.name=d.name;v3.description=d.description;
    v3.inputs=&v3In;v3.outputs=&v3Out;v3.input_count=1;v3.output_count=1;
    v3.gate_cost=d.gate_cost;v3.delay=d.delay;v3.callback=&stressV3;
    return typesV3.register_definition_v3(typesV3.context,&v3);
#else
    const int result=realRegister(context,&d);
    if(result) return result;
    if(realRegister(context,&d)!=-3) return -20;
    d.custom_id++;d.input_count=9;
    if(realRegister(context,&d)!=-2) return -21;
    log("declarative: duplicate and invalid descriptors rejected");
    return 0;
#endif
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* plugin) {
    if(!tc::TCNativeComponent::available(h))return 1;
#ifdef TC_DECLARATIVE_V3
    if(!h->query_service||h->query_service(h->context,TC_SERVICE_COMPONENT_TYPES,
        TC_COMPONENT_TYPES_API_VERSION_2,&typesV3,sizeof(typesV3))!=TC_SERVICE_OK)
        return 2;
#endif
    testHost=*h;realRegister=h->register_component;testHost.register_component=registerStress;
    return byte_adder_example_load(&testHost,plugin);
}
