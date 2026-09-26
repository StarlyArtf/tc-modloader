#include "../src/simulation_read.hpp"
#include "../src/sim_control.hpp"
#include "../src/services.hpp"
#include "../sdk/tc_mod_api.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static int query(void* context,const char* id,uint32_t version,void* out,uint32_t size){return static_cast<tc::Services*>(context)->query(id,version,out,size);}
static int getState(void* context,TCSimulationStateV1* out,uint32_t size){
 if(!context||!out)return TC_SIMULATION_ERR_ARGUMENT;
 if(size<sizeof(*out))return TC_SIMULATION_ERR_ARGUMENT;
 *out={sizeof(*out),TC_SIMULATION_STATE_VERSION_1,
       TC_SIMULATION_STATE_HAS_CYCLE|TC_SIMULATION_STATE_HAS_ENGINE_FRAME|TC_SIMULATION_STATE_HAS_STATE_BUFFER,
       0,7,4242,tc::sim::kStateBufferSize,0};
 return TC_SIMULATION_OK;
}
static int readValue(void* context,uint64_t byteOffset,uint32_t bits,uint64_t* out){
 if(!context||!out)return TC_SIMULATION_ERR_ARGUMENT;
 if(bits<1||bits>64)return TC_SIMULATION_ERR_ARGUMENT;
 if(!tc::sim::offsetInRange(byteOffset))return TC_SIMULATION_ERR_RANGE;
 *out=tc::sim::lowBits(0x1234u,1);   // stands in for the game's reader
 return TC_SIMULATION_OK;
}
/* V2 fakes: the same shape as the runtime's bindings, with the smallest state
   that lets the table's marshalling and error codes be pinned offline. */
static uint64_t fakeRequests=0,fakeClamped=0;static uint32_t fakeSlice=0;
static int64_t fakeLastTarget=0,fakeLastEffective=0;
static int fakeCycle(void*,int64_t* out){*out=7;return TC_SIMULATION_OK;}
static int fakeSize(void*,uint64_t* out){*out=tc::sim::kStateBufferSize;return TC_SIMULATION_OK;}
static int fakeSnapshot(void*,const TCSimChannelV1* channels,uint32_t count,uint64_t* values,
                        uint32_t capacity,int64_t* outCycle,uint32_t* outStable){
 if(!channels||!values||count==0)return TC_SIMULATION_ERR_ARGUMENT;
 if(capacity<count)return TC_SIMULATION_ERR_ARGUMENT;
 for(uint32_t index=0;index<count;++index){
  if(channels[index].bits<1||channels[index].bits>64)return TC_SIMULATION_ERR_ARGUMENT;
  if(!tc::sim::offsetInRange(channels[index].byte_offset))return TC_SIMULATION_ERR_RANGE;
  values[index]=1u;   // stands in for the game's reader
 }
 if(outCycle)*outCycle=7;
 if(outStable)*outStable=1;
 return TC_SIMULATION_OK;
}
static int fakeRequest(void*,int64_t target){++fakeRequests;fakeLastTarget=target;
 fakeLastEffective=fakeSlice?std::min<int64_t>(target,fakeSlice):target;return TC_SIMULATION_OK;}
static int fakeOkay(void*){++fakeRequests;return TC_SIMULATION_OK;}
static int fakeStep(void*,uint32_t cycles){++fakeRequests;fakeLastTarget=cycles;return TC_SIMULATION_OK;}
static int fakeSliceSet(void*,uint32_t cycles){fakeSlice=cycles;return TC_SIMULATION_OK;}
static int fakeControl(void*,TCSimulationControlV1* out,uint32_t size){
 if(!out||size<sizeof(*out))return TC_SIMULATION_ERR_ARGUMENT;
 *out=TCSimulationControlV1{sizeof(*out),TC_SIMULATION_CONTROL_VERSION_1,fakeSlice,0,
                            fakeRequests,fakeClamped,fakeLastTarget,fakeLastEffective,7};
 return TC_SIMULATION_OK;
}
static int fakeFromWire(void*,const TCGameHandle* wire,TCSimWireChannelV1* out,uint32_t size){
 if(!wire||!out||size<sizeof(*out))return TC_SIM_CHANNEL_ERR_ARGUMENT;
 out->byte_offset=256;out->bits=1;out->wire_id=3;
 return TC_SIM_CHANNEL_OK;
}
static int fakeResolve(void*,const TCGameHandle*,TCSimWireChannelV1* channels,uint32_t count,
                       uint32_t* resolved){
 if(!channels||count==0)return TC_SIM_CHANNEL_ERR_ARGUMENT;
 if(resolved)*resolved=count;
 return TC_SIM_CHANNEL_OK;
}
int main(){
 /* Bit and range arithmetic mirrors the game's own read_bits. */
 require(tc::sim::bitMask(1)==1&&tc::sim::bitMask(63)==0x7fffffffffffffffull&&tc::sim::bitMask(64)==~uint64_t{0},"bit mask is wrong");
 require(tc::sim::lowBits(0xffffffffffffffffull,1)==1&&tc::sim::lowBits(0xffffffffffffffffull,64)==0xffffffffffffffffull,"low bits are wrong");
 require(tc::sim::lowBits(0x123456789abcdef0ull,8)==0xf0,"a byte width did not keep the low byte");
 require(tc::sim::offsetInRange(0)&&tc::sim::offsetInRange(tc::sim::kStateBufferSize-8),"a valid state offset was refused");
 require(!tc::sim::offsetInRange(tc::sim::kStateBufferSize-7)&&!tc::sim::offsetInRange(1ull<<40),"an out-of-range state offset was accepted");

 tc::GameHandles handles;tc::Services services(handles);
 TCSimulationApiV1 api{};
 require(services.query(nullptr,1,&api,sizeof(api))==TC_SERVICE_ERR_ARGUMENT,"null id accepted");
 require(services.query(TC_SERVICE_SIMULATION,99,&api,sizeof(api))==TC_SERVICE_ERR_VERSION,"unknown version accepted");
 require(services.query(TC_SERVICE_SIMULATION,1,&api,sizeof(TCServiceHeader))==TC_SERVICE_ERR_SIZE,"short table accepted");
 require(services.query(TC_SERVICE_SIMULATION,TC_SIMULATION_API_VERSION_1,&api,sizeof(api))==TC_SERVICE_OK,"simulation service unavailable");
 require(api.size==sizeof(api)&&api.version==1&&api.context,"bad simulation table header");
 require(services.query(TC_SERVICE_BOARD,1,&api,sizeof(api))==TC_SERVICE_ERR_SIZE,"the board table accepted a simulation-sized buffer");
 TCSimulationStateV1 state{};
 require(tc::simulationState(&api,&state)==TC_SIMULATION_ERR_UNAVAILABLE,"unbound state reader accepted");
 uint64_t value=0;
 require(tc::readSimulationValue(&api,256,1,&value)==TC_SIMULATION_ERR_UNAVAILABLE,"unbound value reader accepted");
 services.bindSimulation(&services,&getState,&readValue);
 require(tc::simulationState(&api,&state)==TC_SIMULATION_OK,"state read failed");
 require(state.size==sizeof(state)&&state.version==TC_SIMULATION_STATE_VERSION_1,"bad state header");
 require(state.cycle==7&&state.engine_frame==4242&&state.state_size==tc::sim::kStateBufferSize,"bad state values");
 require(state.flags==(TC_SIMULATION_STATE_HAS_CYCLE|TC_SIMULATION_STATE_HAS_ENGINE_FRAME|TC_SIMULATION_STATE_HAS_STATE_BUFFER),"bad state flags");
 require(tc::readSimulationValue(&api,256,1,&value)==TC_SIMULATION_OK&&value==0,"value read failed");
 require(api.read_value(api.context,256,0,&value)==TC_SIMULATION_ERR_ARGUMENT,"a zero width was accepted");
 require(api.read_value(api.context,tc::sim::kStateBufferSize,8,&value)==TC_SIMULATION_ERR_RANGE,"an out-of-range offset was accepted");
 require(api.get_state(api.context,&state,sizeof(TCServiceHeader))==TC_SIMULATION_ERR_ARGUMENT,"short state buffer accepted");
 /* The SDK helper has to refuse an old host the same way the other services do. */
 TCHost old{};old.size=static_cast<uint32_t>(offsetof(TCHost,query_service));
 require(tc::simulationService(&old,&api)==TC_SERVICE_ERR_UNAVAILABLE,"old host exposed services");
 TCHost host{};host.size=sizeof(host);host.context=&services;host.query_service=&query;
 std::memset(&api,0,sizeof(api));
require(tc::simulationService(&host,&api)==TC_SERVICE_OK,"SDK helper query failed");

 /* ---- V2: control, consistent reads and the wire-channel service ---------- */
 {
  /* The slice arithmetic is a pure function, so it is pinned here. */
  bool clamped=false;
  using tc::sim_control::clampTarget;
  require(clampTarget(100,10,0,&clamped)==100&&!clamped,"no slice must pass the target through");
  require(clampTarget(100,10,4,&clamped)==14&&clamped,"a slice must shorten the target");
  require(clampTarget(12,10,4,&clamped)==12&&!clamped,"a target inside the slice must survive");
  require(clampTarget(-1,10,4,&clamped)==-1&&!clamped,"a reset target must not be sliced");
  require(clampTarget(100,-1,4,&clamped)==100&&!clamped,"a target before the first run must pass");
  require(clampTarget(INT64_MAX,INT64_MAX-2,4,&clamped)==INT64_MAX&&!clamped,
          "an overflowing slice must not wrap");

 /* The V2 table exists, and its control entries answer only when bound. */
  services.bindSimulationControl(&services,&fakeCycle,&fakeSize,&fakeSnapshot,&fakeRequest,
                                 &fakeRequest,&fakeOkay,&fakeOkay,&fakeStep,&fakeSliceSet,
                                 &fakeControl);
  services.bindSimChannels(&services,&fakeFromWire,&fakeResolve);
 TCSimulationApiV2 api2{};
  require(services.query(TC_SERVICE_SIMULATION,TC_SIMULATION_API_VERSION_2,&api2,sizeof(api2))==TC_SERVICE_OK,
          "V2 table unavailable");
  require(api2.size==sizeof(api2)&&api2.version==TC_SIMULATION_API_VERSION_2&&api2.context,
          "bad V2 table header");
  require(api2.size>=sizeof(TCSimulationApiV1),"V2 must keep the whole V1 prefix");
  int64_t cycle=0;
  uint64_t size=0;
  require(api2.cycle(api2.context,&cycle)==TC_SIMULATION_OK&&cycle==7,"V2 cycle failed");
  require(api2.state_size(api2.context,&size)==TC_SIMULATION_OK&&size==tc::sim::kStateBufferSize,
          "V2 state size failed");
  TCSimChannelV1 channels[2]{};
  channels[0].size=sizeof(TCSimChannelV1);channels[0].version=TCSIM_CHANNEL_VERSION_1;
  channels[0].byte_offset=256;channels[0].bits=1;
  channels[1].size=sizeof(TCSimChannelV1);channels[1].version=TCSIM_CHANNEL_VERSION_1;
  channels[1].byte_offset=264;channels[1].bits=8;
  uint64_t values[2]{};
  uint32_t stable=9;
  require(api2.snapshot(api2.context,channels,2,values,2,&cycle,&stable)==TC_SIMULATION_OK,
          "V2 snapshot failed");
  require(values[0]==1&&values[1]==1,"V2 snapshot values are wrong");
  require(stable==1&&cycle==7,"a snapshot that saw no step must report stable");
  require(api2.snapshot(api2.context,channels,2,values,1,nullptr,nullptr)==TC_SIMULATION_ERR_ARGUMENT,
          "a snapshot smaller than the channel count was accepted");
  TCSimChannelV1 beyond=channels[0];
  beyond.byte_offset=tc::sim::kStateBufferSize;
  require(api2.snapshot(api2.context,&beyond,1,values,2,nullptr,&stable)==TC_SIMULATION_ERR_RANGE,
          "a snapshot with an out-of-range channel was accepted");
  require(api2.run_to(api2.context,40)==TC_SIMULATION_OK,"V2 run_to failed");
  require(api2.run_for(api2.context,5)==TC_SIMULATION_OK,"V2 run_for failed");
  require(api2.pause(api2.context)==TC_SIMULATION_OK,"V2 pause failed");
  require(api2.reset(api2.context)==TC_SIMULATION_OK,"V2 reset failed");
  require(api2.step(api2.context,3)==TC_SIMULATION_OK,"V2 step failed");
  require(api2.set_slice(api2.context,1)==TC_SIMULATION_OK,"V2 set_slice failed");
  TCSimulationControlV1 control{};
  require(api2.control(api2.context,&control,sizeof(control))==TC_SIMULATION_OK,
          "V2 control status failed");
  require(control.slice==1&&control.requests>=5,"V2 control status is empty");
  require(api2.control(api2.context,&control,sizeof(TCServiceHeader))==TC_SIMULATION_ERR_ARGUMENT,
          "a short control buffer was accepted");

  /* A loader without the V2 bindings must answer "unavailable", not garbage. */
  tc::Services bare(handles);
  TCSimulationApiV2 bareApi{};
  require(bare.query(TC_SERVICE_SIMULATION,TC_SIMULATION_API_VERSION_2,&bareApi,sizeof(bareApi))==TC_SERVICE_OK,
          "the V2 table is not published before its bindings");
  int64_t bareCycle=0;
  require(bareApi.cycle(bareApi.context,&bareCycle)==TC_SIMULATION_ERR_UNAVAILABLE,
          "an unbound V2 control entry answered");

  /* The wire-channel service has its own table and its own errors. */
  TCSimChannelApiV1 channelApi{};
  require(services.query(TC_SERVICE_SIM_CHANNEL,TC_SIM_CHANNEL_API_VERSION_1,&channelApi,
                         sizeof(channelApi))==TC_SERVICE_OK,"channel table unavailable");
  require(channelApi.size==sizeof(channelApi)&&channelApi.version==TC_SIM_CHANNEL_API_VERSION_1,
          "bad channel table header");
  TCSimWireChannelV1 wire{};
  const TCGameHandle someWire{};
  wire.size=sizeof(wire);wire.version=TCSIM_WIRE_CHANNEL_VERSION_1;
  require(channelApi.from_wire(channelApi.context,nullptr,&wire,sizeof(wire))==
              TC_SIM_CHANNEL_ERR_ARGUMENT,
          "a null wire handle was accepted");
  require(channelApi.from_wire(channelApi.context,&someWire,&wire,sizeof(wire))==TC_SIM_CHANNEL_OK&&
              wire.bits==1&&wire.byte_offset==256,
          "a wire channel was not resolved");
  uint32_t live=0;
  require(channelApi.resolve(channelApi.context,nullptr,&wire,1,&live)==TC_SIM_CHANNEL_OK&&live==1,
          "a channel list was not re-resolved");
  require(services.query(TC_SERVICE_SIM_CHANNEL,2,&channelApi,sizeof(channelApi))==TC_SERVICE_ERR_VERSION,
          "an unknown channel service version was accepted");
 }
 std::cout<<"PASS simulation service: bit arithmetic, buffer bounds and versioned discovery\n";
}
