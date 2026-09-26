#include "../src/services.hpp"
#include "../sdk/tc_mod_api.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static int query(void* context,const char* id,uint32_t version,void* out,uint32_t size){return static_cast<tc::Services*>(context)->query(id,version,out,size);}
static int capture(void* context,const TCGameHandle* handle,TCBoardSnapshotV1* out,uint32_t size){
 if(!context||!handle||!out)return TC_SNAPSHOT_ERR_ARGUMENT;
 if(size<sizeof(*out))return TC_SNAPSHOT_ERR_SIZE;
 *out={sizeof(*out),TC_BOARD_SNAPSHOT_VERSION_1,*handle,42,99,3,2,1,0,
       TC_BOARD_SNAPSHOT_HAS_ENGINE_FRAME|TC_BOARD_SNAPSHOT_HAS_SIMULATION_CYCLE|TC_BOARD_SNAPSHOT_HAS_SELECTION|TC_BOARD_SNAPSHOT_HAS_PREVIOUS_SELECTION,0};
 return TC_SNAPSHOT_OK;
}
static int captureObjects(void* context,const TCGameHandle* handle,TCBoardObjectSnapshotV1* out,uint32_t size,const TCBoardObjectBuffersV1* buffers){
 if(!context||!handle||!out||!buffers)return TC_SNAPSHOT_ERR_ARGUMENT;
 if(size<sizeof(*out)||buffers->size<sizeof(*buffers))return TC_SNAPSHOT_ERR_SIZE;
 *out={sizeof(*out),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,*handle,42,77,1,1,0,0,
       TC_BOARD_OBJECT_SNAPSHOT_HAS_COMPONENTS|TC_BOARD_OBJECT_SNAPSHOT_HAS_WIRES,0};
 if(buffers->component_capacity<1||buffers->wire_capacity<1)return TC_SNAPSHOT_ERR_CAPACITY;
 buffers->components[0]={sizeof(TCGameHandle),TC_GAME_OBJECT_COMPONENT,77,101};
 buffers->wires[0]={sizeof(TCGameHandle),TC_GAME_OBJECT_WIRE,77,102};
 out->component_written=1;out->wire_written=1;return TC_SNAPSHOT_OK;
}
static const TCGameHandle* lastReadHandle=nullptr;
static int readComponent(void* context,const TCGameHandle* handle,TCComponentInfoV1* out,uint32_t size){
 if(!context||!handle||!out)return TC_SNAPSHOT_ERR_ARGUMENT;
 if(size<sizeof(*out))return TC_SNAPSHOT_ERR_SIZE;
 lastReadHandle=handle;
 *out={sizeof(*out),TC_COMPONENT_INFO_VERSION_1,TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE,0x4e,*handle,0x77,
       -3,9,2,0,0x414E44325F303031ull,0,0};
 return TC_SNAPSHOT_OK;
}
static int readWire(void* context,const TCGameHandle* handle,TCWireInfoV1* out,uint32_t size){
 if(!context||!handle||!out)return TC_SNAPSHOT_ERR_ARGUMENT;
 if(size<sizeof(*out))return TC_SNAPSHOT_ERR_SIZE;
 *out={sizeof(*out),TC_WIRE_INFO_VERSION_1,TC_WIRE_INFO_HAS_ENDPOINT|TC_WIRE_INFO_HAS_WIDTH|TC_WIRE_INFO_HAS_STATE_SLOT,0,
       *handle,4,1,2,3,4,1,0,256,0};
 return TC_SNAPSHOT_OK;
}
static const TCComponentPinBuffersV1* lastPinBuffers=nullptr;
static int readPins(void* context,const TCGameHandle* handle,TCComponentPinsV1* out,uint32_t size,const TCComponentPinBuffersV1* buffers){
 if(!context||!handle||!out||!buffers)return TC_SNAPSHOT_ERR_ARGUMENT;
 if(size<sizeof(*out)||buffers->size<sizeof(*buffers))return TC_SNAPSHOT_ERR_SIZE;
 lastPinBuffers=buffers;
 *out={sizeof(*out),TC_COMPONENT_PINS_VERSION_1,TC_BOARD_PINS_HAS_PROTOTYPE,0x4e,*handle,
       0x414E44325F303031ull,1,1,0,0};
 if(buffers->pin_capacity<2)return TC_SNAPSHOT_ERR_CAPACITY;
 buffers->pins[0]={sizeof(TCPinInfoV1),TC_COMPONENT_PINS_VERSION_1,TC_PIN_INPUT,TC_PIN_INFO_HAS_POSITION,-3,4,1,0,1};
 buffers->pins[1]={sizeof(TCPinInfoV1),TC_COMPONENT_PINS_VERSION_1,TC_PIN_OUTPUT,TC_PIN_INFO_HAS_POSITION|TC_PIN_INFO_WIDTH_AUTO,2,0,0,0,0x7fffffffffffffffull};
 out->pin_written=2;
 return TC_SNAPSHOT_OK;
}
int main(){
 tc::GameHandles handles;tc::Services services(handles);TCBoardApiV1 board{};TCBoardApiV2 board2{};TCBoardApiV3 board3{};TCBoardApiV4 board4{};TCBoardApiV5 board5{};
 require(services.query(nullptr,1,&board,sizeof(board))==TC_SERVICE_ERR_ARGUMENT,"null id accepted");
 require(services.query("tc.missing",1,&board,sizeof(board))==TC_SERVICE_ERR_UNAVAILABLE,"unknown service accepted");
 require(services.query(TC_SERVICE_BOARD,7,&board,sizeof(board))==TC_SERVICE_ERR_VERSION,"unknown version accepted");
 require(services.query(TC_SERVICE_BOARD,1,&board,sizeof(TCServiceHeader))==TC_SERVICE_ERR_SIZE,"short table accepted");
 require(services.query(TC_SERVICE_BOARD,1,&board,sizeof(board))==TC_SERVICE_OK,"board service unavailable");
 require(board.size==sizeof(board)&&board.version==1&&board.context,"bad board service header");
 TCGameHandle handle{};require(board.get_current(board.context,&handle)==TC_HANDLE_ERR_UNAVAILABLE,"empty board returned");
 int value=7;handles.enterBoard(&value,10);
 require(board.get_current(board.context,&handle)==TC_HANDLE_OK,"service did not issue board handle");
 const void* resolved=nullptr;require(board.validate(board.context,&handle)==1,"service rejected live handle");
 require(board.resolve(board.context,&handle,&resolved)==TC_HANDLE_OK&&resolved==&value,"service resolved wrong board");
 require(services.query(TC_SERVICE_BOARD,2,&board2,sizeof(board2))==TC_SERVICE_OK,"board V2 service unavailable");
 require(board2.size==sizeof(board2)&&board2.version==2&&board2.capture_snapshot,"bad board V2 table");
 TCBoardSnapshotV1 snapshot{};
 require(tc::captureBoardSnapshot(&board2,&handle,&snapshot)==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound snapshot provider accepted");
 services.bindBoardSnapshot(&services,&capture);
 require(tc::captureBoardSnapshot(&board2,&handle,&snapshot)==TC_SNAPSHOT_OK,"snapshot capture failed");
 require(snapshot.size==sizeof(snapshot)&&snapshot.version==1&&snapshot.engine_frame==42&&snapshot.simulation_cycle==99,"bad snapshot header");
 require(snapshot.board.generation==handle.generation&&snapshot.selected_component_count==3&&snapshot.selected_wire_count==2,"bad snapshot values");
 require(board2.capture_snapshot(board2.context,&handle,&snapshot,sizeof(TCServiceHeader))==TC_SNAPSHOT_ERR_SIZE,"short snapshot accepted");
 require(services.query(TC_SERVICE_BOARD,3,&board3,sizeof(board3))==TC_SERVICE_OK,"board V3 service unavailable");
 require(board3.size==sizeof(board3)&&board3.version==3&&board3.capture_objects,"bad board V3 table");
 TCBoardObjectSnapshotV1 objects{};TCBoardObjectBuffersV1 empty{sizeof(empty),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,nullptr,0,nullptr,0};
 require(tc::captureBoardObjects(&board3,&handle,&objects,&empty)==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound object provider accepted");
 services.bindBoardObjects(&captureObjects);
 require(tc::captureBoardObjects(&board3,&handle,&objects,&empty)==TC_SNAPSHOT_ERR_CAPACITY&&objects.component_count==1&&objects.wire_count==1,"object count query failed");
 TCGameHandle components[1]{},wires[1]{};TCBoardObjectBuffersV1 storage{sizeof(storage),TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,components,1,wires,1};
 require(tc::captureBoardObjects(&board3,&handle,&objects,&storage)==TC_SNAPSHOT_OK,"object capture failed");
 require(objects.component_written==1&&objects.wire_written==1&&components[0].kind==TC_GAME_OBJECT_COMPONENT&&wires[0].kind==TC_GAME_OBJECT_WIRE,"bad child handles");
 require(services.query(TC_SERVICE_BOARD,4,&board4,sizeof(board4))==TC_SERVICE_OK,"board V4 service unavailable");
 require(board4.size==sizeof(board4)&&board4.version==4,"bad board V4 header");
 require(board4.get_current==board3.get_current&&board4.validate==board3.validate&&board4.resolve==board3.resolve&&
         board4.capture_snapshot==board3.capture_snapshot&&board4.capture_objects==board3.capture_objects,
         "board V4 did not repeat the V3 prefix");
 require(services.query(TC_SERVICE_BOARD,4,&board4,sizeof(TCServiceHeader))==TC_SERVICE_ERR_SIZE,"short V4 table accepted");
 TCComponentInfoV1 component{};TCWireInfoV1 wire{};
 require(tc::readComponent(&board4,&components[0],&component)==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound component reader accepted");
 require(tc::readWire(&board4,&wires[0],&wire)==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound wire reader accepted");
 services.bindBoardReaders(&readComponent,&readWire);
 require(tc::readComponent(&board4,&components[0],&component)==TC_SNAPSHOT_OK,"component read failed");
 require(lastReadHandle==&components[0],"the component handle was not passed through");
 require(component.size==sizeof(component)&&component.version==1&&component.kind==0x4e&&component.id==0x77,"bad component info");
 require(component.x==-3&&component.y==9&&component.rotation==2&&component.custom_prototype_id==0x414E44325F303031ull,"bad component fields");
 require(board4.read_component(board4.context,&components[0],&component,sizeof(TCServiceHeader))==TC_SNAPSHOT_ERR_SIZE,"short component info accepted");
 require(tc::readWire(&board4,&wires[0],&wire)==TC_SNAPSHOT_OK,"wire read failed");
 require(wire.size==sizeof(wire)&&wire.version==1&&wire.id==4&&wire.x1==1&&wire.y1==2&&wire.x2==3&&wire.y2==4&&wire.bit_width==1&&wire.state_byte_offset==256,"bad wire info");
 require(wire.flags==(TC_WIRE_INFO_HAS_ENDPOINT|TC_WIRE_INFO_HAS_WIDTH|TC_WIRE_INFO_HAS_STATE_SLOT),"bad wire flags");
 require(services.query(TC_SERVICE_BOARD,5,&board5,sizeof(board5))==TC_SERVICE_OK,"board V5 service unavailable");
 require(board5.size==sizeof(board5)&&board5.version==5,"bad board V5 header");
 require(board5.get_current==board4.get_current&&board5.validate==board4.validate&&board5.resolve==board4.resolve&&
         board5.capture_snapshot==board4.capture_snapshot&&board5.capture_objects==board4.capture_objects&&
         board5.read_component==board4.read_component&&board5.read_wire==board4.read_wire,
         "board V5 did not repeat the V4 prefix");
 require(services.query(TC_SERVICE_BOARD,5,&board5,sizeof(TCServiceHeader))==TC_SERVICE_ERR_SIZE,"short V5 table accepted");
 TCComponentPinsV1 pins{};TCPinInfoV1 pinStorage[2]{};
 TCComponentPinBuffersV1 emptyPins{sizeof(emptyPins),TC_COMPONENT_PINS_VERSION_1,0,0,nullptr,0};
 require(tc::readComponentPins(&board5,&components[0],&pins,&emptyPins)==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound pin reader accepted");
 services.bindBoardPinReader(&readPins);
 require(tc::readComponentPins(&board5,&components[0],&pins,&emptyPins)==TC_SNAPSHOT_ERR_CAPACITY,"pin count query failed");
 require(lastPinBuffers==&emptyPins,"the pin buffers were not passed through");
 require(pins.input_count==1&&pins.output_count==1&&pins.kind==0x4e&&pins.pin_written==0,"bad pin counts");
 require(pins.custom_prototype_id==0x414E44325F303031ull&&(pins.flags&TC_BOARD_PINS_HAS_PROTOTYPE),"bad pin prototype fields");
 TCComponentPinBuffersV1 pinsStorage{sizeof(pinsStorage),TC_COMPONENT_PINS_VERSION_1,0,0,pinStorage,2};
 require(tc::readComponentPins(&board5,&components[0],&pins,&pinsStorage)==TC_SNAPSHOT_OK,"pin read failed");
 require(pins.pin_written==2&&pinStorage[0].direction==TC_PIN_INPUT&&pinStorage[1].direction==TC_PIN_OUTPUT,"bad pin list");
 require(pinStorage[0].x==-3&&pinStorage[0].y==4&&pinStorage[0].bits==1,"bad input pin values");
 require((pinStorage[1].flags&TC_PIN_INFO_WIDTH_AUTO)&&pinStorage[1].bits==0&&pinStorage[1].word_size_raw==0x7fffffffffffffffull,"bad AUTO_SIZE pin");
 require(board5.read_component_pins(board5.context,&components[0],&pins,sizeof(TCServiceHeader),&pinsStorage)==TC_SNAPSHOT_ERR_SIZE,"short pin header accepted");
 TCBoardApiV6 board6{};TCWireEndsV1 wireEnds{};
 require(services.query(TC_SERVICE_BOARD,6,&board6,sizeof(board6))==TC_SERVICE_OK,"board V6 service unavailable");
 require(board6.size==sizeof(board6)&&board6.version==6,"bad board V6 header");
 require(board6.get_current==board5.get_current&&board6.read_component_pins==board5.read_component_pins&&
         board6.read_component==board5.read_component&&board6.read_wire==board5.read_wire,
         "board V6 did not repeat the V5 prefix");
 require(services.query(TC_SERVICE_BOARD,6,&board6,sizeof(TCServiceHeader))==TC_SERVICE_ERR_SIZE,"short V6 table accepted");
 require(board6.read_wire_ends(board6.context,&wires[0],&wireEnds,sizeof(wireEnds))==TC_SNAPSHOT_ERR_UNAVAILABLE,"unbound wire-end reader accepted");
 handles.leaveBoard();require(board.validate(board.context,&handle)==0,"service kept stale handle valid");
 TCHost old{};old.size=static_cast<uint32_t>(offsetof(TCHost,query_service));
 require(tc::boardService(&old,&board)==TC_SERVICE_ERR_UNAVAILABLE,"old host exposed services");
 TCHost host{};host.size=sizeof(host);host.context=&services;host.query_service=&query;
 std::memset(&board,0,sizeof(board));require(tc::boardService(&host,&board)==TC_SERVICE_OK,"SDK helper query failed");
 TCPinOrderApiV1 pinOrder1{};TCPinOrderApiV2 pinOrder2{};
 require(services.query(TC_SERVICE_PIN_ORDER,1,&pinOrder1,sizeof(pinOrder1))==TC_SERVICE_OK,
         "pin-order V1 service unavailable");
 require(services.query(TC_SERVICE_PIN_ORDER,2,&pinOrder2,sizeof(pinOrder2))==TC_SERVICE_OK,
         "pin-order V2 service unavailable");
 require(pinOrder2.version==2&&pinOrder2.count==pinOrder1.count&&pinOrder2.entry==pinOrder1.entry&&
         pinOrder2.move==pinOrder1.move&&pinOrder2.order==pinOrder1.order&&
         pinOrder2.set_order==pinOrder1.set_order&&pinOrder2.reset==pinOrder1.reset,
         "pin-order V2 did not preserve the V1 prefix");
 require(services.query(TC_SERVICE_PIN_ORDER,3,&pinOrder2,sizeof(pinOrder2))==TC_SERVICE_ERR_VERSION,
         "unknown pin-order version accepted");
 TCComponentStorageApiV1 componentStorage{};
 require(services.query(TC_SERVICE_COMPONENT_STORAGE,1,&componentStorage,sizeof(componentStorage))==
         TC_SERVICE_OK,"component storage service unavailable");
 require(componentStorage.size==sizeof(componentStorage)&&componentStorage.version==1&&
         componentStorage.info&&componentStorage.read_config&&componentStorage.write_config&&
         componentStorage.capture_state&&componentStorage.restore_state,
         "bad component storage table");
TCComponentStorageApiV2 componentStorage2{};
require(services.query(TC_SERVICE_COMPONENT_STORAGE,2,&componentStorage2,sizeof(componentStorage2))==
        TC_SERVICE_OK,"component storage V2 unavailable");
require(componentStorage2.size==sizeof(componentStorage2)&&componentStorage2.version==2&&
        componentStorage2.info&&componentStorage2.read_config&&componentStorage2.write_config&&
        componentStorage2.capture_state&&componentStorage2.restore_state&&
        componentStorage2.begin_edit&&componentStorage2.commit_edit&&componentStorage2.abort_edit,
        "bad component storage V2 table");
require(services.query(TC_SERVICE_COMPONENT_STORAGE,3,&componentStorage2,sizeof(componentStorage2))==
        TC_SERVICE_ERR_VERSION,"unknown component storage version accepted");
 require(services.query(TC_SERVICE_COMPONENT_STORAGE,1,&componentStorage,sizeof(TCServiceHeader))==
         TC_SERVICE_ERR_SIZE,"short component storage table accepted");
 TCPinOrderBoundsV1 reported{sizeof(reported),TC_PIN_ORDER_BOUNDS_VERSION_1,12,
                             TC_PIN_ORDER_GROUP_INPUTS,77,10.f,20.f,30.f,40.f};
 require(pinOrder2.include_bounds(pinOrder2.context,&reported)==TC_PIN_ORDER_OK,
         "pin entry bounds were rejected");
 reported.min_x=5.f;reported.max_y=55.f;
 require(pinOrder2.include_bounds(pinOrder2.context,&reported)==TC_PIN_ORDER_OK,
         "second pin entry bounds were rejected");
 TCPinOrderBoundsV1 combined{};
 require(pinOrder2.bounds(pinOrder2.context,12,TC_PIN_ORDER_GROUP_INPUTS,77,&combined,
                          sizeof(combined))==TC_PIN_ORDER_OK&&
         combined.min_x==5.f&&combined.min_y==20.f&&combined.max_x==30.f&&combined.max_y==55.f,
         "pin entry bounds were not unioned");
 require(pinOrder2.bounds(pinOrder2.context,12,TC_PIN_ORDER_GROUP_INPUTS,78,&combined,
                          sizeof(combined))==TC_PIN_ORDER_ERR_NOT_FOUND,
         "missing pin entry bounds did not report NOT_FOUND");
 std::cout<<"PASS services: exact versions, Board snapshots, object reads and pin-entry layout V2\n";
}
