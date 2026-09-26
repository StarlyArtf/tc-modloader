// Development-only real-game smoke test for tc.component.geometry V1.
#include "../sdk/tc_native_component.h"
#include "../sdk/tc_component_geometry.h"
#include "../sdk/tc_game_model.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kId=0x47454f4d5f303031ULL;
const TCHost* host=nullptr;
tc::component_geometry::Api geometry{};
TCBoardApiV4 board{};
TCCommandApiV2 commands{};
TCGameHandle boardHandle{};
uint64_t request=0;
bool raw=false,done=false;

void logic(TCLogicIO* io){if(io&&io->output_count)io->outputs[0]=0;}
void finish(const std::string& value){
    if(done)return;
    done=true;
    if(host&&host->log)host->log(host->context,value.c_str());
    if(host&&host->data_directory_utf8)
        std::ofstream(std::filesystem::u8path(host->data_directory_utf8)/"result.txt")<<value;
}

void frame(void*,const TCFrame*){
    if(done)return;
    if(!request){
        if(board.get_current(board.context,&boardHandle)!=TC_HANDLE_OK)return;
        TCCommandV2 command{};command.size=sizeof(command);command.type=TC_COMMAND_BOARD_PLACE_COMPONENT;
        command.subject=boardHandle;command.custom_prototype_id=kId;command.kind=0x4e;
        command.x=-60;command.y=40;
        const int status=commands.submit(commands.context,&command,&request);
        if(status!=TC_COMMAND_OK)finish("FAIL geometry placement submit="+std::to_string(status));
        return;
    }
    TCCommandStatusV1 state{};
    if(commands.get_status(commands.context,request,&state,sizeof(state))!=TC_COMMAND_OK)return;
    if(state.state==TC_COMMAND_STATE_QUEUED||state.state==TC_COMMAND_STATE_RUNNING)return;
    if(state.state!=TC_COMMAND_STATE_SUCCEEDED){finish("FAIL geometry placement result="+std::to_string(state.result));return;}
    TCBoardObjectSnapshotV1 snapshot{};snapshot.size=sizeof(snapshot);snapshot.version=TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};buffers.size=sizeof(buffers);buffers.version=TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    int status=board.capture_objects(board.context,&boardHandle,&snapshot,sizeof(snapshot),&buffers);
    if(status!=TC_SNAPSHOT_ERR_CAPACITY)return;
    std::vector<TCGameHandle> components(snapshot.component_count),wires(snapshot.wire_count);
    buffers.components=components.data();buffers.component_capacity=components.size();
    buffers.wires=wires.data();buffers.wire_capacity=wires.size();
    status=board.capture_objects(board.context,&boardHandle,&snapshot,sizeof(snapshot),&buffers);
    if(status!=TC_SNAPSHOT_OK)return;
    for(const auto& handle:components){
        TCComponentInfoV1 info{};info.size=sizeof(info);
        if(board.read_component(board.context,&handle,&info,sizeof(info))!=TC_SNAPSHOT_OK||info.custom_prototype_id!=kId)continue;
        float halfWidth=0,halfHeight=0;
        const int read=tc::component_geometry::readFootprint(geometry,handle,&halfWidth,&halfHeight);
        finish(std::string(read==0&&halfWidth==6.f&&halfHeight==3.f&&raw?"PASS":"FAIL")+
               " component geometry service set=0 packed="+(raw?"ok":"bad")+
               " read="+std::to_string(read)+" half="+std::to_string(halfWidth)+","+std::to_string(halfHeight));
        return;
    }
}
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out){
    if(!h||!out||h->api_version!=TC_MOD_API_VERSION)return 1;
    host=h;
    tc::TCNativeComponent component{};
    component.id=kId;component.name="Geometry probe";component.description="M5 smoke test";
    component.outputs.push_back(TCComponentPin{"out",1});component.callback=&logic;
    const int registration=component.registerWith(h);
    if(registration!=0||!tc::component_geometry::table(h,&geometry)){
        finish("FAIL geometry registration/table status="+std::to_string(registration));return 0;
    }
    const int status=tc::component_geometry::setFootprint(geometry,kId,6.f,3.f);
    tc::TCGameModel game{};tc::TCPrototype prototype{};
    if(status==TC_COMPONENT_GEOMETRY_OK&&game.load(h)&&game.getCustomPrototype(kId,prototype)){
        uint64_t count=0,packed=0;unsigned char* storage=nullptr;
        std::memcpy(&count,prototype.bytes+0x48,sizeof(count));
        std::memcpy(&storage,prototype.bytes+0x50,sizeof(storage));
        if(count==1&&storage){std::memcpy(&packed,storage+8,sizeof(packed));raw=packed==0x0006000cfffdfffaULL;}
        using Destroy=void(*)(void*);
        auto destroy=reinterpret_cast<Destroy>(h->resolve_symbol(h->context,"eqdestroy___modelZboardZprototype95list_u3259"));
        if(destroy)destroy(&prototype);
    }
    if(status!=0||!raw){finish("FAIL component geometry service set="+std::to_string(status)+" packed=bad");return 0;}
    if(tc::boardService(h,&board)!=TC_SERVICE_OK||tc::commandService(h,&commands)!=TC_SERVICE_OK){
        finish("FAIL component geometry board/command service");return 0;
    }
    out->on_frame=&frame;
    return 0;
}
