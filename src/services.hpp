#pragma once
#include "game_handles.hpp"
#include "pin_order.hpp"
#include "pin_order_bounds.hpp"
#include "component_registry.hpp"
#include "native_logic.hpp"
#include "../sdk/tc_service_api.h"
#include <cstring>

namespace tc {
class Services {
    GameHandles& handles;
    using SnapshotCapture=int(*)(void*,const TCGameHandle*,TCBoardSnapshotV1*,uint32_t);
    using ObjectCapture=int(*)(void*,const TCGameHandle*,TCBoardObjectSnapshotV1*,uint32_t,const TCBoardObjectBuffersV1*);
    using ComponentReader=int(*)(void*,const TCGameHandle*,TCComponentInfoV1*,uint32_t);
    using WireReader=int(*)(void*,const TCGameHandle*,TCWireInfoV1*,uint32_t);
    using PinReader=int(*)(void*,const TCGameHandle*,TCComponentPinsV1*,uint32_t,const TCComponentPinBuffersV1*);
    using SimStateReader=int(*)(void*,TCSimulationStateV1*,uint32_t);
    using SimValueReader=int(*)(void*,uint64_t,uint32_t,uint64_t*);
    /* Simulation V2: control, consistent reads, and the channel resolver. */
    using SimCycleReader=int(*)(void*,int64_t*);
    using SimSizeReader=int(*)(void*,uint64_t*);
    using SimSnapshotReader=int(*)(void*,const TCSimChannelV1*,uint32_t,uint64_t*,uint32_t,
                                   int64_t*,uint32_t*);
    using SimRunTo=int(*)(void*,int64_t);
    using SimRunFor=int(*)(void*,int64_t);
    using SimPause=int(*)(void*);
    using SimReset=int(*)(void*);
    using SimStep=int(*)(void*,uint32_t);
    using SimSlice=int(*)(void*,uint32_t);
    using SimControlReader=int(*)(void*,TCSimulationControlV1*,uint32_t);
    using WireChannelReader=int(*)(void*,const TCGameHandle*,TCSimWireChannelV1*,uint32_t);
    using WireChannelResolver=int(*)(void*,const TCGameHandle*,TCSimWireChannelV1*,uint32_t,uint32_t*);
    /* Per-cycle capture (TC_SERVICE_SIM_CAPTURE). */
    using CaptureConfigure=int(*)(void*,const TCSimChannelV1*,uint32_t,uint32_t,
                                  const TCCaptureTriggerV1*);
    using CaptureAction=int(*)(void*);
    using CaptureRead=int(*)(void*,uint64_t*,uint64_t*,uint32_t,uint32_t*);
    using CaptureStatus=int(*)(void*,TCCaptureStatusV1*,uint32_t);
    using WireEndsReader=int(*)(void*,const TCGameHandle*,TCWireEndsV1*,uint32_t);
    /* IO values (TC_SERVICE_IO_VALUE): the game-facing side lives in the
       runtime; this table only forwards, like the services above. */
    using IoEvaluate=int(*)(void*,const char*,uint64_t*);
    using IoFormat=int(*)(void*,uint64_t,uint32_t,uint32_t,char*,uint32_t);
    using IoInputRead=int(*)(void*,const TCGameHandle*,uint64_t,uint64_t*);
    using IoInputWrite=int(*)(void*,const TCGameHandle*,uint64_t,uint64_t);
    using IoInputFlip=int(*)(void*,const TCGameHandle*,uint64_t,uint64_t);
    using IoInputWidth=int(*)(void*,const TCGameHandle*,uint64_t,uint32_t*);
    using IoConstantWrite=int(*)(void*,const TCGameHandle*,uint64_t,uint64_t);
    using IoSlotWrite=int(*)(void*,uint64_t,uint64_t);
    /* Pin order (TC_SERVICE_PIN_ORDER): the store itself lives in the loader,
       next to the build_io_state_view hook that applies it. */
    using PinOrderCount=uint32_t(*)(void*,uint32_t);
    using PinOrderEntry=int(*)(void*,uint32_t,uint32_t,TCPinOrderEntryV1*);
    using PinOrderMove=int(*)(void*,uint32_t,uint32_t,uint32_t);
    using PinOrderOrder=int(*)(void*,uint32_t,uint64_t*,uint32_t,uint32_t*);
    using PinOrderSetOrder=int(*)(void*,uint32_t,const uint64_t*,uint32_t);
    using PinOrderReset=int(*)(void*,uint32_t);
    /* The component catalogue (TC_SERVICE_COMPONENT_REGISTRY): a read-only view
       of what the registration entry points recorded this session. */
    using RegistryCount=uint32_t(*)(void*);
    using RegistryGet=int(*)(void*,uint32_t,TCComponentTypeInfoV1*);
    using RegistryFind=int(*)(void*,uint64_t,TCComponentTypeInfoV1*);
    using RegistryPin=int(*)(void*,uint64_t,uint32_t,uint32_t,TCComponentPinInfoV1*);
    void* snapshotContext=nullptr;
    SnapshotCapture snapshotCapture=nullptr;
    ObjectCapture objectCapture=nullptr;
    ComponentReader componentReader=nullptr;
    WireReader wireReader=nullptr;
    WireEndsReader wireEndsReader=nullptr;
    PinReader pinReader=nullptr;
    void* simulationContext=nullptr;
    SimStateReader simulationState=nullptr;
    SimValueReader simulationValue=nullptr;
    SimCycleReader simulationCycle=nullptr;
    SimSizeReader simulationSize=nullptr;
    SimSnapshotReader simulationSnapshot=nullptr;
    SimRunTo simulationRunTo=nullptr;
    SimRunFor simulationRunFor=nullptr;
    SimPause simulationPause=nullptr;
    SimReset simulationReset=nullptr;
    SimStep simulationStep=nullptr;
    SimSlice simulationSlice=nullptr;
    SimControlReader simulationControl=nullptr;
    WireChannelReader wireChannel=nullptr;
    WireChannelResolver wireChannelResolver=nullptr;
    CaptureConfigure captureConfigure=nullptr;
    CaptureAction captureStart=nullptr;
    CaptureAction captureStop=nullptr;
    CaptureRead captureRead=nullptr;
    CaptureStatus captureStatus=nullptr;
    void* ioValueContext=nullptr;
    IoEvaluate ioEvaluate=nullptr;
    IoFormat ioFormat=nullptr;
    IoInputRead ioReadInput=nullptr;
    IoInputWrite ioWriteInput=nullptr;
    IoInputFlip ioFlipInput=nullptr;
    IoInputWidth ioInputWidth=nullptr;
    IoConstantWrite ioWriteConstant=nullptr;
    IoSlotWrite ioWriteConstantSlot=nullptr;
    static Services* self(void* context){return static_cast<Services*>(context);}
    static int boardCurrent(void* context,TCGameHandle* out){return self(context)->handles.current(TC_GAME_OBJECT_BOARD,out);}
    static int boardValidate(void* context,const TCGameHandle* handle){return self(context)->handles.valid(handle);}
    static int boardResolve(void* context,const TCGameHandle* handle,const void** out){return self(context)->handles.resolve(handle,out);}
    static int boardSnapshot(void* context,const TCGameHandle* handle,TCBoardSnapshotV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->snapshotCapture?service->snapshotCapture(service->snapshotContext,handle,out,outSize):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int boardObjects(void* context,const TCGameHandle* handle,TCBoardObjectSnapshotV1* out,uint32_t outSize,const TCBoardObjectBuffersV1* buffers){
        auto* service=self(context);
        return service->objectCapture?service->objectCapture(service->snapshotContext,handle,out,outSize,buffers):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int boardComponent(void* context,const TCGameHandle* handle,TCComponentInfoV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->componentReader?service->componentReader(service->snapshotContext,handle,out,outSize):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int boardWire(void* context,const TCGameHandle* handle,TCWireInfoV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->wireReader?service->wireReader(service->snapshotContext,handle,out,outSize):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int boardComponentPins(void* context,const TCGameHandle* handle,TCComponentPinsV1* out,uint32_t outSize,const TCComponentPinBuffersV1* buffers){
        auto* service=self(context);
        return service->pinReader?service->pinReader(service->snapshotContext,handle,out,outSize,buffers):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int boardWireEnds(void* context,const TCGameHandle* handle,TCWireEndsV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->wireEndsReader?service->wireEndsReader(service->snapshotContext,handle,out,outSize):TC_SNAPSHOT_ERR_UNAVAILABLE;
    }
    static int simulationGetState(void* context,TCSimulationStateV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->simulationState?service->simulationState(service->simulationContext,out,outSize):TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationReadValue(void* context,uint64_t byteOffset,uint32_t bits,uint64_t* out){
        auto* service=self(context);
        return service->simulationValue?service->simulationValue(service->simulationContext,byteOffset,bits,out):TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationCycleCall(void* context,int64_t* out){
        auto* service=self(context);
        return service->simulationCycle?service->simulationCycle(service->simulationContext,out):TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationStateSizeCall(void* context,uint64_t* out){
        auto* service=self(context);
        return service->simulationSize?service->simulationSize(service->simulationContext,out):TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationSnapshotCall(void* context,const TCSimChannelV1* channels,uint32_t count,
                                      uint64_t* values,uint32_t capacity,int64_t* outCycle,
                                      uint32_t* outStable){
        auto* service=self(context);
        return service->simulationSnapshot
                   ?service->simulationSnapshot(service->simulationContext,channels,count,values,
                                                capacity,outCycle,outStable)
                   :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationRunToCall(void* context,int64_t target){
        auto* service=self(context);
        return service->simulationRunTo?service->simulationRunTo(service->simulationContext,target)
                                       :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationRunForCall(void* context,int64_t cycles){
        auto* service=self(context);
        return service->simulationRunFor?service->simulationRunFor(service->simulationContext,cycles)
                                        :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationPauseCall(void* context){
        auto* service=self(context);
        return service->simulationPause?service->simulationPause(service->simulationContext)
                                       :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationResetCall(void* context){
        auto* service=self(context);
        return service->simulationReset?service->simulationReset(service->simulationContext)
                                       :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationStepCall(void* context,uint32_t cycles){
        auto* service=self(context);
        return service->simulationStep?service->simulationStep(service->simulationContext,cycles)
                                      :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationSliceCall(void* context,uint32_t cycles){
        auto* service=self(context);
        return service->simulationSlice?service->simulationSlice(service->simulationContext,cycles)
                                       :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int simulationControlCall(void* context,TCSimulationControlV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->simulationControl
                   ?service->simulationControl(service->simulationContext,out,outSize)
                   :TC_SIMULATION_ERR_UNAVAILABLE;
    }
    static int wireChannelCall(void* context,const TCGameHandle* wire,TCSimWireChannelV1* out,
                               uint32_t outSize){
        auto* service=self(context);
        return service->wireChannel
                   ?service->wireChannel(service->simulationContext,wire,out,outSize)
                   :TC_SIM_CHANNEL_ERR_UNAVAILABLE;
    }
    static int wireChannelResolveCall(void* context,const TCGameHandle* board,
                                      TCSimWireChannelV1* channels,uint32_t count,
                                      uint32_t* resolved){
        auto* service=self(context);
        return service->wireChannelResolver
                   ?service->wireChannelResolver(service->simulationContext,board,channels,count,
                                                 resolved)
                   :TC_SIM_CHANNEL_ERR_UNAVAILABLE;
    }
    static int captureConfigureCall(void* context,const TCSimChannelV1* channels,uint32_t count,
                                    uint32_t depth,const TCCaptureTriggerV1* trigger){
        auto* service=self(context);
        return service->captureConfigure
                   ?service->captureConfigure(service->simulationContext,channels,count,depth,
                                              trigger)
                   :TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    }
    static int captureStartCall(void* context){
        auto* service=self(context);
        return service->captureStart?service->captureStart(service->simulationContext)
                                    :TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    }
    static int captureStopCall(void* context){
        auto* service=self(context);
        return service->captureStop?service->captureStop(service->simulationContext)
                                   :TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    }
    static int captureReadCall(void* context,uint64_t* cycles,uint64_t* values,uint32_t capacity,
                               uint32_t* rows){
        auto* service=self(context);
        return service->captureRead
                   ?service->captureRead(service->simulationContext,cycles,values,capacity,rows)
                   :TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    }
    static int captureStatusCall(void* context,TCCaptureStatusV1* out,uint32_t outSize){
        auto* service=self(context);
        return service->captureStatus
                   ?service->captureStatus(service->simulationContext,out,outSize)
                   :TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    }
    static int ioEvaluateCall(void* context,const char* expression,uint64_t* out){
        auto* service=self(context);
        return service->ioEvaluate?service->ioEvaluate(service->ioValueContext,expression,out):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioFormatCall(void* context,uint64_t value,uint32_t width,uint32_t format,char* out,uint32_t outSize){
        auto* service=self(context);
        return service->ioFormat?service->ioFormat(service->ioValueContext,value,width,format,out,outSize):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioReadInputCall(void* context,const TCGameHandle* board,uint64_t index,uint64_t* out){
        auto* service=self(context);
        return service->ioReadInput?service->ioReadInput(service->ioValueContext,board,index,out):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioWriteInputCall(void* context,const TCGameHandle* board,uint64_t index,uint64_t value){
        auto* service=self(context);
        return service->ioWriteInput?service->ioWriteInput(service->ioValueContext,board,index,value):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioFlipInputCall(void* context,const TCGameHandle* board,uint64_t index,uint64_t bit){
        auto* service=self(context);
        return service->ioFlipInput?service->ioFlipInput(service->ioValueContext,board,index,bit):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioInputWidthCall(void* context,const TCGameHandle* board,uint64_t index,uint32_t* out){
        auto* service=self(context);
        return service->ioInputWidth?service->ioInputWidth(service->ioValueContext,board,index,out):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioWriteConstantCall(void* context,const TCGameHandle* board,uint64_t index,uint64_t value){
        auto* service=self(context);
        return service->ioWriteConstant?service->ioWriteConstant(service->ioValueContext,board,index,value):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static int ioWriteConstantSlotCall(void* context,uint64_t component,uint64_t value){
        auto* service=self(context);
        return service->ioWriteConstantSlot?service->ioWriteConstantSlot(service->ioValueContext,component,value):TC_IO_VALUE_ERR_UNAVAILABLE;
    }
    static uint32_t pinOrderCountCall(void*,uint32_t group){
        return tc::pin_order::store().count(group);
    }
    static int pinOrderEntryCall(void*,uint32_t group,uint32_t index,TCPinOrderEntryV1* out){
        tc::pin_order::Entry entry{};
        const int status=tc::pin_order::store().entry(group,index,&entry);
        if(status!=tc::pin_order::status::ok)return status;
        if(!out)return tc::pin_order::status::argument;
        *out=TCPinOrderEntryV1{};
        out->key=entry.key;
        out->width=entry.width;
        const std::size_t copied=std::min(entry.name.size(),sizeof(out->name)-1);
        std::memcpy(out->name,entry.name.data(),copied);
        out->name[copied]=0;
        out->name_size=static_cast<uint32_t>(copied);
        return tc::pin_order::status::ok;
    }
    static int pinOrderMoveCall(void*,uint32_t group,uint32_t from,uint32_t to){
        return tc::pin_order::store().move(group,from,to);
    }
    static int pinOrderOrderCall(void*,uint32_t group,uint64_t* keys,uint32_t capacity,uint32_t* count){
        return tc::pin_order::store().order(group,keys,capacity,count);
    }
    static int pinOrderSetOrderCall(void*,uint32_t group,const uint64_t* keys,uint32_t count){
        return tc::pin_order::store().setOrder(group,keys,count);
    }
    static int pinOrderResetCall(void*,uint32_t group){
        return tc::pin_order::store().clear(group);
    }
    static int pinOrderIncludeBoundsCall(void*,const TCPinOrderBoundsV1* bounds){
        if(!bounds||bounds->size<sizeof(*bounds)||bounds->version!=TC_PIN_ORDER_BOUNDS_VERSION_1)
            return TC_PIN_ORDER_ERR_ARGUMENT;
        return tc::pin_order_bounds::store().include(
            {bounds->frame,bounds->group,bounds->key,bounds->min_x,bounds->min_y,
             bounds->max_x,bounds->max_y});
    }
    static int pinOrderBoundsCall(void*,int32_t frame,uint32_t group,uint64_t key,
                                  TCPinOrderBoundsV1* out,uint32_t outSize){
        if(!out||outSize<sizeof(*out))return TC_PIN_ORDER_ERR_ARGUMENT;
        tc::pin_order_bounds::Bounds found{};
        const int status=tc::pin_order_bounds::store().get(frame,group,key,&found);
        if(status!=TC_PIN_ORDER_OK)return status;
        *out=TCPinOrderBoundsV1{sizeof(*out),TC_PIN_ORDER_BOUNDS_VERSION_1,found.frame,
            found.group,found.key,found.min_x,found.min_y,found.max_x,found.max_y};
        return TC_PIN_ORDER_OK;
    }
    /* One catalogue entry as the ABI struct: strings are copied into the fixed
       fields and truncated there, which is what the struct's capacities are. */
    static void fillTypeInfo(const tc::component_registry::Type& type,TCComponentTypeInfoV1* out){
        *out=TCComponentTypeInfoV1{};
        out->size=sizeof(TCComponentTypeInfoV1);
        out->custom_id=type.custom_id;
        out->gate_cost=type.gate_cost;
        out->delay=type.delay;
        out->implementation=type.implementation;
        out->capabilities=type.capabilities;
        out->schema_version=type.schema_version;
        out->active=type.active?1u:0u;
        out->input_count=static_cast<uint32_t>(type.inputs.size());
        out->output_count=static_cast<uint32_t>(type.outputs.size());
        out->pin_limit=static_cast<uint32_t>(tc::component_registry::kMaxPinsPerDirection);
        const std::string id=tc::component_registry::typeId(type);
        tc::component_registry::copyField(out->type_id,sizeof(out->type_id),id);
        tc::component_registry::copyField(out->name,sizeof(out->name),type.name);
        tc::component_registry::copyField(out->description,sizeof(out->description),type.description);
        tc::component_registry::copyField(out->owner_mod,sizeof(out->owner_mod),type.owner_mod);
        tc::component_registry::copyField(out->status,sizeof(out->status),type.status);
    }
    static uint32_t componentRegistryCountCall(void*){
        return tc::component_registry::count();
    }
    static int componentRegistryGetCall(void*,uint32_t index,TCComponentTypeInfoV1* out){
        if(!out||out->size<sizeof(*out))return TC_COMPONENT_REGISTRY_ERR_ARGUMENT;
        const std::vector<tc::component_registry::Type> all=tc::component_registry::snapshot();
        if(index>=all.size())return TC_COMPONENT_REGISTRY_ERR_RANGE;
        fillTypeInfo(all[index],out);
        return TC_COMPONENT_REGISTRY_OK;
    }
    static int componentRegistryFindCall(void*,uint64_t custom_id,TCComponentTypeInfoV1* out){
        if(!out||out->size<sizeof(*out))return TC_COMPONENT_REGISTRY_ERR_ARGUMENT;
        tc::component_registry::Type type;
        if(!tc::component_registry::find(custom_id,&type))return TC_COMPONENT_REGISTRY_ERR_UNKNOWN;
        fillTypeInfo(type,out);
        return TC_COMPONENT_REGISTRY_OK;
    }
    static int componentRegistryPinCall(void*,uint64_t custom_id,uint32_t direction,uint32_t index,
                                        TCComponentPinInfoV1* out){
        if(!out||out->size<sizeof(*out))return TC_COMPONENT_REGISTRY_ERR_ARGUMENT;
        if(direction>TC_COMPONENT_PIN_OUTPUT)return TC_COMPONENT_REGISTRY_ERR_ARGUMENT;
        tc::component_registry::Type type;
        if(!tc::component_registry::find(custom_id,&type))return TC_COMPONENT_REGISTRY_ERR_UNKNOWN;
        const auto& pins=direction==TC_COMPONENT_PIN_INPUT?type.inputs:type.outputs;
        if(index>=pins.size())return TC_COMPONENT_REGISTRY_ERR_RANGE;
        *out=TCComponentPinInfoV1{};
        out->size=sizeof(TCComponentPinInfoV1);
        out->pin_id=(static_cast<uint64_t>(direction)<<32)|index;
        out->direction=direction;
        out->bits=static_cast<uint32_t>(pins[index].bits);
        tc::component_registry::copyField(out->name,sizeof(out->name),pins[index].name);
        return TC_COMPONENT_REGISTRY_OK;
    }
    /* tc.component.instances: the live-instance table.  The calls go straight to
       the logic layer's free functions, which own the bindings. */
    static int componentInstancesEnumerateCall(void*,uint64_t custom_id,
                                               TCComponentInstanceHandle* out,uint32_t capacity,
                                               uint32_t* written,uint32_t* total){
        return tc::logic::instanceEnumerate(custom_id,out,capacity,written,total);
    }
    static int componentInstancesValidateCall(void*,const TCComponentInstanceHandle* handle){
        return tc::logic::instanceValidate(handle);
    }
    static int componentInstancesInfoCall(void*,const TCComponentInstanceHandle* handle,
                                          TCComponentInstanceInfoV1* out){
        return tc::logic::instanceInfo(handle,out);
    }
    static int componentInstancesStateCall(void*,const TCComponentInstanceHandle* handle,
                                           uint64_t* out,uint32_t capacity,uint32_t* words){
        return tc::logic::instanceState(handle,out,capacity,words);
    }
    static int componentInstancesResetCall(void*,const TCComponentInstanceHandle* handle){
        return tc::logic::instanceReset(handle);
    }
    /* tc.component.storage: generation-checked whole-blob configuration and
       simulation-state snapshots owned by the same live binding. */
    static int componentStorageInfoCall(void*,const TCComponentInstanceHandle* handle,
                                        TCComponentStorageInfoV1* out){
        return tc::logic::storageInfo(handle,out);
    }
    static int componentStorageReadConfigCall(void*,const TCComponentInstanceHandle* handle,
                                              void* out,uint32_t capacity,uint32_t* bytes){
        return tc::logic::storageReadConfig(handle,out,capacity,bytes);
    }
    static int componentStorageWriteConfigCall(void*,const TCComponentInstanceHandle* handle,
                                               uint32_t schema,const void* data,uint32_t bytes){
        return tc::logic::storageWriteConfig(handle,schema,data,bytes);
    }
    static int componentStorageCaptureStateCall(void*,const TCComponentInstanceHandle* handle,
                                                void* out,uint32_t capacity,uint32_t* bytes){
        return tc::logic::storageCaptureState(handle,out,capacity,bytes);
    }
    static int componentStorageRestoreStateCall(void*,const TCComponentInstanceHandle* handle,
                                                const void* data,uint32_t bytes){
        return tc::logic::storageRestoreState(handle,data,bytes);
    }
    /* V2 adds the explicit edit transaction (plan §9.3: a batch tool says where
       its action begins and ends instead of the host guessing from frames). */
    static int componentStorageBeginEditCall(void*,const TCComponentInstanceHandle* handle){
        return tc::logic::storageBeginEdit(handle);
    }
    static int componentStorageCommitEditCall(void*,const TCComponentInstanceHandle* handle){
        return tc::logic::storageCommitEdit(handle);
    }
    static int componentStorageAbortEditCall(void*,const TCComponentInstanceHandle* handle){
        return tc::logic::storageAbortEdit(handle);
    }
public:
    explicit Services(GameHandles& value):handles(value){}
    void bindBoardSnapshot(void* context,SnapshotCapture capture){snapshotContext=context;snapshotCapture=capture;}
    void bindBoardObjects(ObjectCapture capture){objectCapture=capture;}
    /* V4 reads go through the same context the snapshot captures already use:
       the runtime owns both the Board registry and the object tables. */
    void bindBoardReaders(ComponentReader component,WireReader wire){componentReader=component;wireReader=wire;}
    void bindBoardPinReader(PinReader pins){pinReader=pins;}
    void bindBoardWireEndsReader(WireEndsReader ends){wireEndsReader=ends;}
    void bindSimulation(void* context,SimStateReader state,SimValueReader value){simulationContext=context;simulationState=state;simulationValue=value;}
    /* V2 and the wire-channel resolver are bound separately so a loader build
       (or a test) that only has V1 still answers V1 honestly. */
    void bindSimulationControl(void* context,SimCycleReader cycle,SimSizeReader size,
                               SimSnapshotReader snapshot,SimRunTo runTo,SimRunFor runFor,
                               SimPause pause,SimReset reset,SimStep step,SimSlice slice,
                               SimControlReader control){
        simulationContext=context;simulationCycle=cycle;simulationSize=size;
        simulationSnapshot=snapshot;simulationRunTo=runTo;simulationRunFor=runFor;
        simulationPause=pause;simulationReset=reset;simulationStep=step;
        simulationSlice=slice;simulationControl=control;
    }
    void bindSimChannels(void* context,WireChannelReader fromWire,
                         WireChannelResolver resolve){
        simulationContext=context;wireChannel=fromWire;wireChannelResolver=resolve;
    }
    void bindSimCapture(void* context,CaptureConfigure configure,CaptureAction start,
                        CaptureAction stop,CaptureRead read,CaptureStatus status){
        simulationContext=context;captureConfigure=configure;captureStart=start;
        captureStop=stop;captureRead=read;captureStatus=status;
    }
    void bindIoValue(void* context,IoEvaluate evaluate,IoFormat format,IoInputRead read,IoInputWrite write,
                     IoInputFlip flip,IoInputWidth width,IoConstantWrite constant,IoSlotWrite slot){
        ioValueContext=context;ioEvaluate=evaluate;ioFormat=format;ioReadInput=read;ioWriteInput=write;
        ioFlipInput=flip;ioInputWidth=width;ioWriteConstant=constant;ioWriteConstantSlot=slot;
    }
    int query(const char* id,uint32_t version,void* out,uint32_t outSize){
        if(!id||!out)return TC_SERVICE_ERR_ARGUMENT;
        if(std::strcmp(id,TC_SERVICE_IO_VALUE)==0){
            if(version!=TC_IO_VALUE_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCIoValueApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCIoValueApiV1*>(out)=TCIoValueApiV1{sizeof(TCIoValueApiV1),TC_IO_VALUE_API_VERSION_1,this,
                &ioEvaluateCall,&ioFormatCall,&ioReadInputCall,&ioWriteInputCall,&ioFlipInputCall,
                &ioInputWidthCall,&ioWriteConstantCall,&ioWriteConstantSlotCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_SIMULATION)==0){
            if(version==TC_SIMULATION_API_VERSION_1){
                if(outSize<sizeof(TCSimulationApiV1))return TC_SERVICE_ERR_SIZE;
                *static_cast<TCSimulationApiV1*>(out)=TCSimulationApiV1{sizeof(TCSimulationApiV1),TC_SIMULATION_API_VERSION_1,this,&simulationGetState,&simulationReadValue};
                return TC_SERVICE_OK;
            }
            if(version==TC_SIMULATION_API_VERSION_2){
                if(outSize<sizeof(TCSimulationApiV2))return TC_SERVICE_ERR_SIZE;
                *static_cast<TCSimulationApiV2*>(out)=TCSimulationApiV2{
                    sizeof(TCSimulationApiV2),TC_SIMULATION_API_VERSION_2,this,
                    &simulationGetState,&simulationReadValue,
                    &simulationCycleCall,&simulationStateSizeCall,&simulationSnapshotCall,
                    &simulationRunToCall,&simulationRunForCall,&simulationPauseCall,
                    &simulationResetCall,&simulationStepCall,&simulationSliceCall,
                    &simulationControlCall};
                return TC_SERVICE_OK;
            }
            return TC_SERVICE_ERR_VERSION;
        }
        if(std::strcmp(id,TC_SERVICE_SIM_CHANNEL)==0){
            if(version!=TC_SIM_CHANNEL_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCSimChannelApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCSimChannelApiV1*>(out)=TCSimChannelApiV1{
                sizeof(TCSimChannelApiV1),TC_SIM_CHANNEL_API_VERSION_1,this,
                &wireChannelCall,&wireChannelResolveCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_SIM_CAPTURE)==0){
            if(version!=TC_SIM_CAPTURE_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCCaptureApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCCaptureApiV1*>(out)=TCCaptureApiV1{
                sizeof(TCCaptureApiV1),TC_SIM_CAPTURE_API_VERSION_1,this,
                &captureConfigureCall,&captureStartCall,&captureStopCall,&captureReadCall,
                &captureStatusCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_PIN_ORDER)==0){
            if(version==TC_PIN_ORDER_API_VERSION_1){
                if(outSize<sizeof(TCPinOrderApiV1))return TC_SERVICE_ERR_SIZE;
                *static_cast<TCPinOrderApiV1*>(out)=TCPinOrderApiV1{sizeof(TCPinOrderApiV1),TC_PIN_ORDER_API_VERSION_1,this,
                    &pinOrderCountCall,&pinOrderEntryCall,&pinOrderMoveCall,&pinOrderOrderCall,
                    &pinOrderSetOrderCall,&pinOrderResetCall};
                return TC_SERVICE_OK;
            }
            if(version==TC_PIN_ORDER_API_VERSION_2){
                if(outSize<sizeof(TCPinOrderApiV2))return TC_SERVICE_ERR_SIZE;
                *static_cast<TCPinOrderApiV2*>(out)=TCPinOrderApiV2{sizeof(TCPinOrderApiV2),TC_PIN_ORDER_API_VERSION_2,this,
                    &pinOrderCountCall,&pinOrderEntryCall,&pinOrderMoveCall,&pinOrderOrderCall,
                    &pinOrderSetOrderCall,&pinOrderResetCall,&pinOrderIncludeBoundsCall,
                    &pinOrderBoundsCall};
                return TC_SERVICE_OK;
            }
            return TC_SERVICE_ERR_VERSION;
        }
        if(std::strcmp(id,TC_SERVICE_COMPONENT_REGISTRY)==0){
            if(version!=TC_COMPONENT_REGISTRY_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCComponentRegistryApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCComponentRegistryApiV1*>(out)=TCComponentRegistryApiV1{sizeof(TCComponentRegistryApiV1),TC_COMPONENT_REGISTRY_API_VERSION_1,this,
                &componentRegistryCountCall,&componentRegistryGetCall,&componentRegistryFindCall,
                &componentRegistryPinCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_COMPONENT_INSTANCES)==0){
            if(version!=TC_COMPONENT_INSTANCES_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCComponentInstancesApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCComponentInstancesApiV1*>(out)=TCComponentInstancesApiV1{
                sizeof(TCComponentInstancesApiV1),TC_COMPONENT_INSTANCES_API_VERSION_1,this,
                &componentInstancesEnumerateCall,&componentInstancesValidateCall,
                &componentInstancesInfoCall,&componentInstancesStateCall,
                &componentInstancesResetCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_COMPONENT_STORAGE)==0){
            if(version==TC_COMPONENT_STORAGE_API_VERSION_2){
                if(outSize<sizeof(TCComponentStorageApiV2))return TC_SERVICE_ERR_SIZE;
                *static_cast<TCComponentStorageApiV2*>(out)=TCComponentStorageApiV2{
                    sizeof(TCComponentStorageApiV2),TC_COMPONENT_STORAGE_API_VERSION_2,this,
                    &componentStorageInfoCall,&componentStorageReadConfigCall,
                    &componentStorageWriteConfigCall,&componentStorageCaptureStateCall,
                    &componentStorageRestoreStateCall,&componentStorageBeginEditCall,
                    &componentStorageCommitEditCall,&componentStorageAbortEditCall};
                return TC_SERVICE_OK;
            }
            if(version!=TC_COMPONENT_STORAGE_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
            if(outSize<sizeof(TCComponentStorageApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCComponentStorageApiV1*>(out)=TCComponentStorageApiV1{
                sizeof(TCComponentStorageApiV1),TC_COMPONENT_STORAGE_API_VERSION_1,this,
                &componentStorageInfoCall,&componentStorageReadConfigCall,
                &componentStorageWriteConfigCall,&componentStorageCaptureStateCall,
                &componentStorageRestoreStateCall};
            return TC_SERVICE_OK;
        }
        if(std::strcmp(id,TC_SERVICE_BOARD)!=0)return TC_SERVICE_ERR_UNAVAILABLE;
        if(version==TC_BOARD_API_VERSION_1){
            if(outSize<sizeof(TCBoardApiV1))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV1*>(out)=TCBoardApiV1{sizeof(TCBoardApiV1),TC_BOARD_API_VERSION_1,this,&boardCurrent,&boardValidate,&boardResolve};
            return TC_SERVICE_OK;
        }
        if(version==TC_BOARD_API_VERSION_2){
            if(outSize<sizeof(TCBoardApiV2))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV2*>(out)=TCBoardApiV2{sizeof(TCBoardApiV2),TC_BOARD_API_VERSION_2,this,&boardCurrent,&boardValidate,&boardResolve,&boardSnapshot};
            return TC_SERVICE_OK;
        }
        if(version==TC_BOARD_API_VERSION_3){
            if(outSize<sizeof(TCBoardApiV3))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV3*>(out)=TCBoardApiV3{sizeof(TCBoardApiV3),TC_BOARD_API_VERSION_3,this,&boardCurrent,&boardValidate,&boardResolve,&boardSnapshot,&boardObjects};
            return TC_SERVICE_OK;
        }
        if(version==TC_BOARD_API_VERSION_4){
            if(outSize<sizeof(TCBoardApiV4))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV4*>(out)=TCBoardApiV4{sizeof(TCBoardApiV4),TC_BOARD_API_VERSION_4,this,&boardCurrent,&boardValidate,&boardResolve,&boardSnapshot,&boardObjects,&boardComponent,&boardWire};
            return TC_SERVICE_OK;
        }
        if(version==TC_BOARD_API_VERSION_5){
            if(outSize<sizeof(TCBoardApiV5))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV5*>(out)=TCBoardApiV5{sizeof(TCBoardApiV5),TC_BOARD_API_VERSION_5,this,&boardCurrent,&boardValidate,&boardResolve,&boardSnapshot,&boardObjects,&boardComponent,&boardWire,&boardComponentPins};
            return TC_SERVICE_OK;
        }
        if(version==TC_BOARD_API_VERSION_6){
            if(outSize<sizeof(TCBoardApiV6))return TC_SERVICE_ERR_SIZE;
            *static_cast<TCBoardApiV6*>(out)=TCBoardApiV6{sizeof(TCBoardApiV6),TC_BOARD_API_VERSION_6,this,&boardCurrent,&boardValidate,&boardResolve,&boardSnapshot,&boardObjects,&boardComponent,&boardWire,&boardComponentPins,&boardWireEnds};
            return TC_SERVICE_OK;
        }
        return TC_SERVICE_ERR_VERSION;
    }
};
}
