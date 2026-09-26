/* Development probe for the Board handle registry (sdk/tc_handle_api.h).

   Read-only, the same way tests/hook-chain-probe.cpp is: it hooks nothing by
   itself and asks the host only.  What it exists for is the half of the
   contract a unit test cannot reach - the *real* level lifetime:

     * the main menu has no board, so the query has to answer UNAVAILABLE;
     * entering a level issues a handle whose resolve() is the very board the
       LEVEL_LOAD event carries;
     * leaving the level invalidates that handle (0 / STALE) and the query goes
       back to UNAVAILABLE;
     * the next level gets a different generation and token, so a handle kept
       from the first level can never resolve into the second one.

   The probe prints one PROBE: line per transition.  tests/hook-chain-probe.cpp
   shows the other half of the pattern: the playtest build (TC_HANDLE_PROBE_DRIVER)
   compiles tests/game-handle-probe-driver.hpp into the same package, and that
   driver enters a board, leaves it and enters another one, because no external
   process can click a borderless fullscreen window. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_event.h"
#include "../sdk/tc_handle_api.h"
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef TC_HANDLE_PROBE_DRIVER
#include "game-handle-probe-driver.hpp"
#endif

static const TCHost* host;
static TCBoardApiV3 boardApi{};
static TCBoardApiV4 boardApi4{};
static TCBoardApiV5 boardApi5{};
static TCBoardApiV6 boardApi6{};
static TCSimulationApiV1 simulationApi{};
static TCCommandApiV1 commandApi{};
static TCCommandApiV2 commandEditApi{};
static TCGameLifecycleApiV1 lifecycleApi{};
static TCTransactionApiV1 transactionApi{};
static TCGameHandle board{};
static TCGameHandle firstWireHandle{};
static bool sawAvailable = false, haveHandle = false, loggedMenu = false;
static int levelLoads = 0, sceneChanges = 0, resolvedMatches = 0, staleAfterScene = 0;
static int snapshots = 0;
static TCGameHandle frameChild{};
static int childFrame=-1;
static bool haveFrameChild=false,reportedChildStale=false;
static uint64_t commandRequest=0;
static bool commandPending=false;
static uint64_t transactionRequest=0;
static bool transactionPending=false;
static bool transactionWanted=false;
static bool transactionEverCompleted=false,editSubmitted=false;
static TCGameHandle transactionBoard{};
static int lifecycleEntered=0,lifecycleLeft=0;
static int lifecycleObjectsChanged=0,lifecycleSelectionChanged=0;
static uint64_t editRequest=0;
static bool editPending=false,editReported=false;
static bool editCountReported=false;
static size_t editLiveBefore=0,editLiveAfter=0;
static int editState=-1,editResult=0;
static uint64_t lastObjectGeneration=0,lastObjectComponents=0,lastObjectWires=0;
static int objectSamples=0;
static bool sawObjectCounts=false;
/* The last engine frame the loader called us for: it dates every event, which is
   how "the level loaded and the scene switched in the same frame" is told from
   "the player left the level several frames later". */
static int lastFrame = -1;

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

#ifdef TC_HANDLE_PROBE_DRIVER
static void logLine(const char* message) {
    if (host && host->log) host->log(host->context, message);
}
#endif

static std::string hex64(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

static const char* handleResult(int status) {
    switch (status) {
        case TC_HANDLE_OK: return "ok";
        case TC_HANDLE_ERR_UNAVAILABLE: return "unavailable";
        case TC_HANDLE_ERR_ARGUMENT: return "argument";
        case TC_HANDLE_ERR_KIND: return "kind";
        case TC_HANDLE_ERR_STALE: return "stale";
        default: return "unknown";
    }
}
static std::string hexByte(uint32_t value) {
    char text[8];
    std::snprintf(text,sizeof(text),"0x%02x",value);
    return text;
}
static int currentBoard(TCGameHandle* out){return boardApi.get_current(boardApi.context,out);}
/* Live components only: the game leaves zero-filled tombstones behind when an
   edit is undone, so the sequence length is not the component count. */
static size_t liveComponents() {
    TCGameHandle board{};
    if (currentBoard(&board) != TC_HANDLE_OK) return 0;
    TCBoardObjectSnapshotV1 counts{};
    TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                                     nullptr, 0, nullptr, 0};
    if (tc::captureBoardObjects(&boardApi, &board, &counts, &countOnly) != TC_SNAPSHOT_ERR_CAPACITY) return 0;
    std::vector<TCGameHandle> handles(counts.component_count), wires(counts.wire_count);
    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
        handles.empty() ? nullptr : handles.data(), handles.size(),
        wires.empty() ? nullptr : wires.data(), wires.size()};
    if (tc::captureBoardObjects(&boardApi, &board, &counts, &storage) != TC_SNAPSHOT_OK) return 0;
    size_t live = 0;
    for (const auto& child : handles) {
        TCComponentInfoV1 info{};
        if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
        if (info.kind) ++live;
    }
    return live;
}

static int validateBoard(const TCGameHandle* value){return boardApi.validate(boardApi.context,value);}
static int resolveBoard(const TCGameHandle* value,const void** out){return boardApi.resolve(boardApi.context,value,out);}

static void onLifecycle(TCGameLifecycleEventV1* event){
    if(!event)return;
    if(event->kind==TC_LIFECYCLE_BOARD_ENTERED)++lifecycleEntered;
    if(event->kind==TC_LIFECYCLE_BOARD_LEFT)++lifecycleLeft;
    if(event->kind==TC_LIFECYCLE_OBJECTS_CHANGED)++lifecycleObjectsChanged;
    if(event->kind==TC_LIFECYCLE_SELECTION_CHANGED)++lifecycleSelectionChanged;
    report("PROBE: lifecycle kind="+std::to_string(event->kind)+
           " sequence="+std::to_string(event->sequence)+
           " frame="+std::to_string(event->engine_frame)+
           " board-valid="+std::to_string(validateBoard(&event->board))+
           " components="+std::to_string(event->component_count)+
           " wires="+std::to_string(event->wire_count)+
           " selected="+std::to_string(event->selected_component_count)+"/"+std::to_string(event->selected_wire_count));
}

static void onEvent(TCEvent* event) {
    if (tc::events::is(event, TC_EVENT_LEVEL_LOAD)) {
        ++levelLoads;
        if (haveHandle)
            report("PROBE: previous handle valid=" +
                   std::to_string(validateBoard(&board)) +
                   " after the new level load");
        void* subject = tc::events::levelBoardModel(event);
        const char* name = tc::events::levelName(event);
        report("PROBE: level.load name=" + std::string(name ? name : "(none)") +
               " subject=0x" + hex64(reinterpret_cast<uint64_t>(subject)) +
               " frame=" + std::to_string(lastFrame));
        TCGameHandle issued{};
        const int status = currentBoard(&issued);
        if (status != TC_HANDLE_OK) {
            report(std::string("PROBE: level handle unavailable (") + handleResult(status) + ")");
            return;
        }
        board = issued;
        haveHandle = true;
        const void* raw = nullptr;
        const int resolvedStatus = resolveBoard(&board, &raw);
        const bool match = resolvedStatus == TC_HANDLE_OK && raw == subject;
        if (match) ++resolvedMatches;
        report("PROBE: level handle generation=" + std::to_string(board.generation) +
               " token=" + std::to_string(board.token) +
               " size=" + std::to_string(board.size) +
               " resolve=" + std::string(handleResult(resolvedStatus)) +
               " resolve-match=" + (match ? "1" : "0") +
               " valid=" + std::to_string(validateBoard(&board)));
        TCBoardSnapshotV1 snapshot{};
        const int snapshotStatus=tc::captureBoardSnapshot(&boardApi,&board,&snapshot);
        if(snapshotStatus==TC_SNAPSHOT_OK&&snapshot.board.generation==board.generation)++snapshots;
        report("PROBE: snapshot status="+std::to_string(snapshotStatus)+
               " version="+std::to_string(snapshot.version)+
               " frame="+std::to_string(snapshot.engine_frame)+
               " cycle="+std::to_string(snapshot.simulation_cycle)+
               " selected-components="+std::to_string(snapshot.selected_component_count)+
               " selected-wires="+std::to_string(snapshot.selected_wire_count)+
               " flags="+std::to_string(snapshot.flags));
        TCBoardObjectSnapshotV1 objects{};
        TCBoardObjectBuffersV1 countOnly{sizeof(countOnly),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,nullptr,0,nullptr,0};
        int objectStatus=tc::captureBoardObjects(&boardApi,&board,&objects,&countOnly);
        std::vector<TCGameHandle> components(objects.component_count),wires(objects.wire_count);
        TCBoardObjectBuffersV1 storage{sizeof(storage),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
            components.empty()?nullptr:components.data(),components.size(),
            wires.empty()?nullptr:wires.data(),wires.size()};
        if(objectStatus==TC_SNAPSHOT_ERR_CAPACITY||objectStatus==TC_SNAPSHOT_OK)
            objectStatus=tc::captureBoardObjects(&boardApi,&board,&objects,&storage);
        size_t resolvedChildren=0;
        for(const auto& child:components){const void* value=nullptr;if(resolveBoard(&child,&value)==TC_HANDLE_OK&&value)++resolvedChildren;}
        for(const auto& child:wires){const void* value=nullptr;if(resolveBoard(&child,&value)==TC_HANDLE_OK&&value)++resolvedChildren;}
        if(!components.empty()){frameChild=components.front();childFrame=lastFrame;haveFrameChild=true;reportedChildStale=false;}
        else if(!wires.empty()){frameChild=wires.front();childFrame=lastFrame;haveFrameChild=true;reportedChildStale=false;}
        if(!wires.empty())firstWireHandle=wires.front();
        report("PROBE: objects status="+std::to_string(objectStatus)+
               " components="+std::to_string(objects.component_count)+
               " wires="+std::to_string(objects.wire_count)+
               " written="+std::to_string(objects.component_written+objects.wire_written)+
               " resolved="+std::to_string(resolvedChildren)+
               " generation="+std::to_string(objects.snapshot_generation));
        /* Board V4: read every object the V3 enumeration just issued.  The
           counters below are what the playtest asserts, so a wrong offset shows
           up as a failed invariant instead of a plausible-looking number. */
        std::set<uint64_t> componentIds;
        size_t componentReads=0,componentUnique=0,componentNamed=0;
        TCComponentInfoV1 firstComponent{};
        std::string componentHeads;
        std::map<uint32_t,size_t> componentKinds;
        size_t componentIndex=0;
        for(const auto& child:components){
            TCComponentInfoV1 info{};
            if(tc::readComponent(&boardApi4,&child,&info)!=TC_SNAPSHOT_OK){++componentIndex;continue;}
            ++componentReads;
            if(componentIds.insert(info.id).second)++componentUnique;
            if(info.flags&TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE)++componentNamed;
            ++componentKinds[info.kind];
            if(componentReads==1)firstComponent=info;
            /* Raw evidence for the record header, so a wrong base address or a
               sentinel element is visible instead of only looking like a bad
               number in the summary line. */
            if(componentIndex<3){
                const void* raw=nullptr;
                if(resolveBoard(&child,&raw)==TC_HANDLE_OK&&raw){
                    const auto* bytes=static_cast<const unsigned char*>(raw);
                    std::string head;
                    for(size_t j=0;j<0x20;++j){
                        char text[4];std::snprintf(text,sizeof(text),"%02x",bytes[j]);
                        head+=text;
                    }
                    componentHeads+=" ["+std::to_string(componentIndex)+"]="+head;
                }
            }
            ++componentIndex;
        }
        if(!componentHeads.empty())report("PROBE: component heads"+componentHeads);
        std::string kindHistogram;
        for(const auto& [kind,count]:componentKinds)
            kindHistogram+=(kindHistogram.empty()?"":" ")+std::to_string(kind)+":"+std::to_string(count);
        report("PROBE: component kinds "+(kindHistogram.empty()?"(none)":kindHistogram));
        size_t wireReads=0,wireWithWidth=0,wireWithSlot=0,wireEndpoints=0;
        TCWireInfoV1 firstWire{};
        for(size_t i=0;i<wires.size();++i){
            TCWireInfoV1 info{};
            if(tc::readWire(&boardApi4,&wires[i],&info)!=TC_SNAPSHOT_OK)continue;
            ++wireReads;
            if(info.id!=i)continue;
            if(info.flags&TC_WIRE_INFO_HAS_WIDTH)++wireWithWidth;
            if(info.flags&TC_WIRE_INFO_HAS_STATE_SLOT)++wireWithSlot;
            if(info.flags&TC_WIRE_INFO_HAS_ENDPOINT)++wireEndpoints;
            if(wireReads==1)firstWire=info;
        }
        report("PROBE: component info status="+std::to_string(componentReads==components.size()?TC_SNAPSHOT_OK:TC_SNAPSHOT_ERR_STALE)+
               " read="+std::to_string(componentReads)+
               " of="+std::to_string(components.size())+
               " unique-ids="+std::to_string(componentUnique)+
               " custom="+std::to_string(componentNamed)+
               " kind0="+std::to_string(firstComponent.kind)+
               " xy0="+std::to_string(firstComponent.x)+","+std::to_string(firstComponent.y)+
               " rotation0="+std::to_string(firstComponent.rotation)+
               " id0="+std::to_string(firstComponent.id));
        /* Board V5: a handle is enough to learn the component's pins.  The
           placeholder entry (kind 0) has no prototype and must be refused, so
           "read" is expected to stay below the component count. */
        size_t pinsRead=0,pinsCount=0,pinsAuto=0,pinsWithPrototype=0,pinsWithoutPins=0,pinLines=0;
        for(size_t index=0;index<components.size();++index){
            TCComponentPinsV1 pinInfo{};
            TCComponentPinBuffersV1 countOnly{sizeof(countOnly),TC_COMPONENT_PINS_VERSION_1,0,0,nullptr,0};
            const int counted=tc::readComponentPins(&boardApi5,&components[index],&pinInfo,&countOnly);
            if(counted!=TC_SNAPSHOT_ERR_CAPACITY&&counted!=TC_SNAPSHOT_OK)continue;
            ++pinsWithPrototype;
            const size_t total=static_cast<size_t>(pinInfo.input_count+pinInfo.output_count);
            pinsCount+=total;
            if(!total){++pinsWithoutPins;continue;}
            std::vector<TCPinInfoV1> storage(total);
            TCComponentPinBuffersV1 buffers{sizeof(buffers),TC_COMPONENT_PINS_VERSION_1,0,0,storage.data(),storage.size()};
            if(tc::readComponentPins(&boardApi5,&components[index],&pinInfo,&buffers)!=TC_SNAPSHOT_OK)continue;
            for(const auto& pin:storage){
                ++pinsRead;
                if(pin.flags&TC_PIN_INFO_WIDTH_AUTO)++pinsAuto;
            }
            /* One line per component keeps the evidence readable and the
               playtest assertions line-oriented; it is bounded by the level's
               object count, which the probe already logs. */
            if(pinLines<8){
                ++pinLines;
                std::string shape="PROBE: pins "+std::to_string(index)+" kind="+hexByte(pinInfo.kind)+
                    " in="+std::to_string(pinInfo.input_count)+" out="+std::to_string(pinInfo.output_count);
                for(size_t p=0;p<storage.size();++p){
                    shape+=std::string(" p")+std::to_string(p)+
                        (storage[p].direction==TC_PIN_INPUT?"=i":"=o")+
                        "("+std::to_string(storage[p].x)+","+std::to_string(storage[p].y)+
                        ",w"+std::to_string(storage[p].bits)+
                        ((storage[p].flags&TC_PIN_INFO_WIDTH_AUTO)?"a":"")+")";
                }
                report(shape);
            }
        }
        report("PROBE: component pins status="+std::to_string(pinsWithPrototype==components.size()?TC_SNAPSHOT_OK:TC_SNAPSHOT_ERR_UNAVAILABLE)+
               " read="+std::to_string(pinsWithPrototype)+
               " of="+std::to_string(components.size())+
               " pins="+std::to_string(pinsRead)+
               " expected="+std::to_string(pinsCount)+
               " zero="+std::to_string(pinsWithoutPins)+
               " auto="+std::to_string(pinsAuto));
        report("PROBE: wire info status="+std::to_string(wireReads==wires.size()?TC_SNAPSHOT_OK:TC_SNAPSHOT_ERR_STALE)+
               " read="+std::to_string(wireReads)+
               " of="+std::to_string(wires.size())+
               " endpoint="+std::to_string(wireEndpoints)+
               " width="+std::to_string(wireWithWidth)+
               " slot="+std::to_string(wireWithSlot)+
               " xy0="+std::to_string(firstWire.x1)+","+std::to_string(firstWire.y1)+"->"+std::to_string(firstWire.x2)+","+std::to_string(firstWire.y2)+
               " width0="+std::to_string(firstWire.bit_width)+
               " slot0="+std::to_string(firstWire.state_byte_offset));
        /* tc.simulation: the cycle the game reports, and the value of the first
           wire read through the slot the V4 record carries.  The two reads below
           must agree with the mask rule the waveform probes are verified with. */
        TCSimulationStateV1 simState{};
        const int stateStatus=tc::simulationState(&simulationApi,&simState);
        report("PROBE: sim state status="+std::to_string(stateStatus)+
               " cycle="+std::to_string(simState.cycle)+
               " frame="+std::to_string(simState.engine_frame)+
               " state-size="+std::to_string(simState.state_size)+
               " flags="+std::to_string(simState.flags));
        if((firstWire.flags&TC_WIRE_INFO_HAS_STATE_SLOT)&&(firstWire.flags&TC_WIRE_INFO_HAS_WIDTH)){
            uint64_t rawValue=0,maskedValue=0;
            const int rawStatus=tc::readSimulationValue(&simulationApi,firstWire.state_byte_offset,64,&rawValue);
            const int maskedStatus=tc::readSimulationValue(&simulationApi,firstWire.state_byte_offset,firstWire.bit_width,&maskedValue);
            const uint64_t expected=firstWire.bit_width>=64?rawValue:(rawValue&((static_cast<uint64_t>(1)<<firstWire.bit_width)-1));
            report("PROBE: sim value status="+std::to_string(maskedStatus)+
                   " raw-status="+std::to_string(rawStatus)+
                   " slot="+std::to_string(firstWire.state_byte_offset)+
                   " width="+std::to_string(firstWire.bit_width)+
                   " value="+std::to_string(maskedValue)+
                   " raw="+std::to_string(rawValue)+
                   " masked="+std::to_string(expected)+
                   " agree="+std::to_string(maskedStatus==TC_SIMULATION_OK&&maskedValue==expected));
        }
        /* Board V6: which pin sits on each end of that wire.  The resolved
           components are read back through V4 so the report shows the kinds the
           geometry actually landed on. */
        if(!wires.empty()){
            TCWireEndsV1 ends{};
            const int endsStatus=tc::readWireEnds(&boardApi6,&firstWireHandle,&ends);
            std::string detail;
            for(int endIndex=0;endIndex<2;++endIndex){
                const TCWireEndV1& end=ends.ends[endIndex];
                uint32_t kind=0xffffffffu;
                if(end.flags&TC_WIRE_END_HAS_COMPONENT){
                    TCComponentInfoV1 info{};
                    if(tc::readComponent(&boardApi4,&end.component,&info)==TC_SNAPSHOT_OK)kind=info.kind;
                }
                detail+=" end"+std::to_string(endIndex)+"=("+std::to_string(end.x)+","+std::to_string(end.y)+
                        ",dir"+(end.flags&TC_WIRE_END_HAS_COMPONENT?std::to_string(end.direction):std::string("none"))+
                        ",pin"+std::to_string(end.pin_index)+
                        ",kind="+hexByte(kind)+")";
            }
            report("PROBE: wire ends status="+std::to_string(endsStatus)+detail);
        }
        TCCommandV1 stop{sizeof(stop),TC_COMMAND_SIM_STOP,0,0,board,0};
        const int commandStatus=commandApi.submit(commandApi.context,&stop,&commandRequest);
        commandPending=commandStatus==TC_COMMAND_OK;
        report("PROBE: command submit="+std::to_string(commandStatus)+" request="+std::to_string(commandRequest));
        transactionBoard=board;transactionWanted=true;
        TCGameHandle wrong{sizeof(TCGameHandle), TC_GAME_OBJECT_COMPONENT, 0, 0};
        report(std::string("PROBE: component handle kind guard=") +
               handleResult(tc::currentGameHandle(host, TC_GAME_OBJECT_COMPONENT, &wrong)));
        return;
    }
    if (tc::events::is(event, TC_EVENT_SCENE_CHANGE)) {
        ++sceneChanges;
        /* The loader's detour invalidates the registry before it raises this
           event, so even a listener sees the old handle refused. */
        const int valid = haveHandle ? validateBoard(&board) : -99;
        report("PROBE: scene.change scene=" + std::to_string(static_cast<int>(event->flags)) +
               " frame=" + std::to_string(lastFrame) +
               " old-handle-valid=" + std::to_string(valid));
        if (haveHandle && valid == 0) ++staleAfterScene;
        if (haveHandle) {
            const void* raw = nullptr;
            report(std::string("PROBE: old handle resolve=") +
                   handleResult(resolveBoard(&board, &raw)) +
                   " pointer=" + (raw ? "set" : "cleared"));
        }
        TCGameHandle now{};
        const int status = currentBoard(&now);
        report(std::string("PROBE: current after scene change=") + handleResult(status));
    }
}

static void onFrame(void*, const TCFrame* frame) {
    if (frame) lastFrame = frame->frame_number;
    /* The edit finishes at the end of the frame that submitted it; the lifecycle
       event lands at the start of the next one, so the counters are reported one
       frame later. */
    if(editPending){
        TCCommandStatusV1 status{};
        const int queried=commandEditApi.get_status(commandEditApi.context,editRequest,&status,sizeof(status));
        if(queried==TC_COMMAND_OK&&(status.state==TC_COMMAND_STATE_SUCCEEDED||status.state==TC_COMMAND_STATE_FAILED||status.state==TC_COMMAND_STATE_CANCELLED)){
            editPending=false;editReported=true;
            editState=static_cast<int>(status.state);editResult=status.result;
            editLiveAfter=liveComponents();
            report("PROBE: edit state="+std::to_string(status.state)+
                   " result="+std::to_string(status.result)+
                   " live-before="+std::to_string(editLiveBefore)+
                   " live-after="+std::to_string(editLiveAfter));
        }
    } else if(editReported&&!editCountReported){
        editCountReported=true;
        report("PROBE: lifecycle objects-changed="+std::to_string(lifecycleObjectsChanged)+
               " selection-changed="+std::to_string(lifecycleSelectionChanged)+
               " entered="+std::to_string(lifecycleEntered)+
               " left="+std::to_string(lifecycleLeft));
    }
    /* Count the board's objects every frame and log only when something
       changes: this is what tells "the level is still being built" apart from
       "the game replaced the board", which the level.load moment alone cannot. */
    if(frame){
        TCGameHandle sampled{};
        if(currentBoard(&sampled)==TC_HANDLE_OK){
            TCBoardObjectSnapshotV1 counts{};
            TCBoardObjectBuffersV1 none{sizeof(none),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,nullptr,0,nullptr,0};
            const int counted=tc::captureBoardObjects(&boardApi,&sampled,&counts,&none);
            if(counted==TC_SNAPSHOT_ERR_CAPACITY||counted==TC_SNAPSHOT_OK){
                if(!sawObjectCounts||sampled.generation!=lastObjectGeneration||
                   counts.component_count!=lastObjectComponents||counts.wire_count!=lastObjectWires){
                    sawObjectCounts=true;
                    lastObjectGeneration=sampled.generation;
                    lastObjectComponents=counts.component_count;
                    lastObjectWires=counts.wire_count;
                    if(objectSamples++<16)
                        report("PROBE: board objects frame="+std::to_string(frame->frame_number)+
                               " generation="+std::to_string(sampled.generation)+
                               " components="+std::to_string(counts.component_count)+
                               " wires="+std::to_string(counts.wire_count)+
                               " status="+std::to_string(counted));
                }
            }
        }
    }
    if(transactionWanted&&!transactionPending&&validateBoard(&transactionBoard)==1){
        TCCommandV1 stop{sizeof(stop),TC_COMMAND_SIM_STOP,0,0,transactionBoard,0};
        int transactionStatus=transactionApi.begin(transactionApi.context,&transactionBoard,TC_TRANSACTION_SAVE_ON_COMMIT,&transactionRequest);
        if(transactionStatus==TC_TRANSACTION_OK)transactionStatus=transactionApi.stage(transactionApi.context,transactionRequest,&stop);
        if(transactionStatus==TC_TRANSACTION_OK)transactionStatus=transactionApi.commit(transactionApi.context,transactionRequest);
        transactionPending=transactionStatus==TC_TRANSACTION_OK;transactionWanted=false;
        report("PROBE: transaction submit="+std::to_string(transactionStatus)+" request="+std::to_string(transactionRequest));
    }
    if(frame&&haveFrameChild&&!reportedChildStale&&frame->frame_number!=childFrame){
        reportedChildStale=true;
        report("PROBE: child handle next-frame valid="+std::to_string(validateBoard(&frameChild)));
        /* The V4 reads have to refuse the same handle the registry just
           invalidated, otherwise a Mod could still dereference a record the
           board no longer owns. */
        TCComponentInfoV1 component{};
        TCWireInfoV1 wire{};
        const int componentRead=tc::readComponent(&boardApi4,&frameChild,&component);
        const int wireRead=tc::readWire(&boardApi4,&frameChild,&wire);
        report("PROBE: child read next-frame component="+std::to_string(componentRead)+
               " wire="+std::to_string(wireRead));
        /* The level.load moment is not necessarily the settled board: enumerate
           again on this later frame and compare, so a partially built sequence
           is visible instead of being mistaken for a decode bug. */
        TCGameHandle settled{};
        if(currentBoard(&settled)==TC_HANDLE_OK){
            TCBoardObjectSnapshotV1 objects{};
            TCBoardObjectBuffersV1 countOnly{sizeof(countOnly),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,nullptr,0,nullptr,0};
            int objectStatus=tc::captureBoardObjects(&boardApi,&settled,&objects,&countOnly);
            std::vector<TCGameHandle> settledComponents(objects.component_count),settledWires(objects.wire_count);
            TCBoardObjectBuffersV1 storage{sizeof(storage),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                settledComponents.empty()?nullptr:settledComponents.data(),settledComponents.size(),
                settledWires.empty()?nullptr:settledWires.data(),settledWires.size()};
            if(objectStatus==TC_SNAPSHOT_ERR_CAPACITY||objectStatus==TC_SNAPSHOT_OK)
                objectStatus=tc::captureBoardObjects(&boardApi,&settled,&objects,&storage);
            std::map<uint32_t,size_t> kinds;
            std::set<uint64_t> ids;
            size_t readable=0,uniqueIds=0,empty=0;
            for(const auto& child:settledComponents){
                TCComponentInfoV1 info{};
                if(tc::readComponent(&boardApi4,&child,&info)!=TC_SNAPSHOT_OK)continue;
                ++readable;++kinds[info.kind];
                if(!info.kind)++empty;
                if(ids.insert(info.id).second)++uniqueIds;
            }
            std::string kindHistogram;
            for(const auto& [kind,count]:kinds)
                kindHistogram+=(kindHistogram.empty()?"":" ")+std::to_string(kind)+":"+std::to_string(count);
            report("PROBE: settled objects status="+std::to_string(objectStatus)+
                   " components="+std::to_string(objects.component_count)+
                   " wires="+std::to_string(objects.wire_count)+
                   " read="+std::to_string(readable)+
                   " unique-ids="+std::to_string(uniqueIds)+
                   " empty="+std::to_string(empty)+
                   " kinds="+(kindHistogram.empty()?std::string("(none)"):kindHistogram));
        }
    }
    if(commandPending){
        TCCommandStatusV1 status{};
        const int queried=commandApi.get_status(commandApi.context,commandRequest,&status,sizeof(status));
        if(queried==TC_COMMAND_OK&&(status.state==TC_COMMAND_STATE_SUCCEEDED||status.state==TC_COMMAND_STATE_FAILED||status.state==TC_COMMAND_STATE_CANCELLED)){
            report("PROBE: command complete state="+std::to_string(status.state)+
                   " result="+std::to_string(status.result)+
                   " submitted="+std::to_string(status.submitted_frame)+
                   " completed="+std::to_string(status.completed_frame));
            commandPending=false;
        }
    }
    if(transactionPending){
        TCTransactionStatusV1 status{};
        const int queried=transactionApi.get_status(transactionApi.context,transactionRequest,&status,sizeof(status));
        if(queried==TC_TRANSACTION_OK&&(status.state==TC_TRANSACTION_STATE_COMMITTED||status.state==TC_TRANSACTION_STATE_FAILED||status.state==TC_TRANSACTION_STATE_ABORTED||status.state==TC_TRANSACTION_STATE_CONFLICT)){
            report("PROBE: transaction complete state="+std::to_string(status.state)+
                   " result="+std::to_string(status.result)+
                   " staged="+std::to_string(status.staged_count)+
                   " completed="+std::to_string(status.completed_count));
            transactionPending=false;
            transactionEverCompleted=true;
        }
    }
    /* Only now, with the probe's own transaction finished and the board settled,
       change the board once so the lifecycle service has something to report.
       Submitting this any earlier would make the stop+save transaction conflict
       with it, and reading "before" during level.load would read a board that is
       still being built. */
    if(transactionEverCompleted&&!editSubmitted&&validateBoard(&transactionBoard)==1){
        editSubmitted=true;
        editLiveBefore=liveComponents();
        TCCommandV2 place{};
        place.size=sizeof(place);
        place.type=TC_COMMAND_BOARD_PLACE_COMPONENT;
        place.subject=transactionBoard;
        place.kind=0x04;
        place.x=0;
        place.y=-12;
        const int submitted=commandEditApi.submit(commandEditApi.context,&place,&editRequest);
        editPending=submitted==TC_COMMAND_OK;
        report("PROBE: edit submit="+std::to_string(submitted)+
               " request="+std::to_string(editRequest)+
               " live-before="+std::to_string(editLiveBefore));
    }
#ifdef TC_HANDLE_PROBE_DRIVER
    /* The driver only has this one per-frame entry point: the loader calls the
       plugin, and the plugin forwards. */
    tc_handle_driver::tick(frame);
#else
    (void)frame;
#endif
    TCGameHandle now{};
    const int status = currentBoard(&now);
    if (status == TC_HANDLE_OK) {
        if (!sawAvailable) {
            sawAvailable = true;
            report("PROBE: Board available generation=" + std::to_string(now.generation) +
                   " token=" + std::to_string(now.token));
        }
        return;
    }
    if (!loggedMenu) {
        loggedMenu = true;
        report(std::string("PROBE: Board unavailable on the main menu (") +
               handleResult(status) + ")");
    }
    if (sawAvailable) {
        sawAvailable = false;
        report(std::string("PROBE: Board unavailable again (") + handleResult(status) + ")");
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!out->size || out->size < sizeof(TCPlugin)) return 2;
    if (!tc::hostHas(h, TC_CAP_GAME_HANDLES) || !tc::hostHas(h, TC_CAP_EVENTS) ||
        !tc::hostHas(h, TC_CAP_SERVICES)) return 3;
    if (tc::boardService(h,&boardApi) != TC_SERVICE_OK || boardApi.version != TC_BOARD_API_VERSION_3) return 4;
    if (tc::boardService(h,&boardApi4) != TC_SERVICE_OK || boardApi4.version != TC_BOARD_API_VERSION_4) return 11;
    if (tc::boardService(h,&boardApi5) != TC_SERVICE_OK || boardApi5.version != TC_BOARD_API_VERSION_5) return 12;
    if (tc::boardService(h,&boardApi6) != TC_SERVICE_OK || boardApi6.version != TC_BOARD_API_VERSION_6) return 14;
    if (tc::simulationService(h,&simulationApi) != TC_SERVICE_OK || simulationApi.version != TC_SIMULATION_API_VERSION_1) return 13;
    if (tc::commandService(h,&commandApi) != TC_SERVICE_OK || commandApi.version != TC_COMMAND_API_VERSION_1) return 7;
    if (tc::commandService(h,&commandEditApi) != TC_SERVICE_OK || commandEditApi.version != TC_COMMAND_API_VERSION_2) return 15;
    if (tc::lifecycleService(h,&lifecycleApi) != TC_SERVICE_OK || lifecycleApi.version != TC_LIFECYCLE_API_VERSION_1) return 8;
    if (lifecycleApi.subscribe(lifecycleApi.context,TC_LIFECYCLE_BOARD_ENTERED|TC_LIFECYCLE_BOARD_LEFT|
        TC_LIFECYCLE_OBJECTS_CHANGED|TC_LIFECYCLE_SELECTION_CHANGED,&onLifecycle,nullptr)!=TC_LIFECYCLE_OK) return 9;
    if (tc::transactionService(h,&transactionApi) != TC_SERVICE_OK || transactionApi.version != TC_TRANSACTION_API_VERSION_1) return 10;
    if (tc::events::subscribe(h, TC_EVENT_LEVEL_LOAD | TC_EVENT_SCENE_CHANGE, &onEvent, nullptr) !=
        TC_EVENT_OK) return 5;
    out->on_frame = &onFrame;
    report("Game handle probe: Board v3/v4, command/lifecycle/transaction v1 armed");
    report("PROBE: board v4="+std::to_string(boardApi4.version)+
           " prefix="+std::to_string(boardApi4.get_current==boardApi.get_current&&
                                     boardApi4.capture_snapshot==boardApi.capture_snapshot&&
                                     boardApi4.capture_objects==boardApi.capture_objects));
    report("PROBE: board v5="+std::to_string(boardApi5.version)+
           " prefix="+std::to_string(boardApi5.get_current==boardApi.get_current&&
                                     boardApi5.capture_objects==boardApi.capture_objects&&
                                     boardApi5.read_component==boardApi4.read_component&&
                                     boardApi5.read_wire==boardApi4.read_wire));
    report("PROBE: board v6="+std::to_string(boardApi6.version)+
           " prefix="+std::to_string(boardApi6.get_current==boardApi.get_current&&
                                     boardApi6.read_component==boardApi4.read_component&&
                                     boardApi6.read_component_pins==boardApi5.read_component_pins));
#ifdef TC_HANDLE_PROBE_DRIVER
    if (!tc_handle_driver::start(h, &logLine)) {
        report("Game handle probe: the driver could not be installed");
        return 6;
    }
#endif
    return 0;
}
