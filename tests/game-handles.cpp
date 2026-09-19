#include "../src/game_handles.hpp"
#include <stdexcept>
#include <iostream>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
 tc::GameHandles handles;TCGameHandle first{},second{};const void* resolved=nullptr;
 require(handles.current(TC_GAME_OBJECT_BOARD,&first)==TC_HANDLE_ERR_UNAVAILABLE,"empty registry accepted");
 int boardA=1,boardB=2;handles.enterBoard(&boardA,10);
 require(handles.current(TC_GAME_OBJECT_BOARD,&first)==TC_HANDLE_OK,"current board unavailable");
 require(handles.resolve(&first,&resolved)==TC_HANDLE_OK&&resolved==&boardA,"first board did not resolve");
 require(handles.valid(&first)==1,"live board reported stale");
 TCGameHandle wrong=first;wrong.kind=TC_GAME_OBJECT_WIRE;
 require(handles.resolve(&wrong,&resolved)==TC_HANDLE_ERR_KIND,"wrong handle kind accepted");
 handles.enterBoard(&boardB,20);
 require(handles.valid(&first)==0,"old generation remained valid");
 require(handles.current(TC_GAME_OBJECT_BOARD,&second)==TC_HANDLE_OK,"second board unavailable");
 require(handles.resolve(&second,&resolved)==TC_HANDLE_OK&&resolved==&boardB,"second board did not resolve");
 handles.leaveBoard();
 require(handles.valid(&second)==0,"handle survived scene leave");
 require(handles.current(TC_GAME_OBJECT_COMPONENT,&first)==TC_HANDLE_ERR_KIND,"reserved kind accepted");
 /* The real game loads the level and switches to the board scene in the same
    frame, so that switch is the entry, not a leave; a switch in a later frame
    is the leave that has to invalidate. */
 TCGameHandle third{};int boardC=3;
 handles.enterBoard(&boardC,30);
 require(handles.current(TC_GAME_OBJECT_BOARD,&third)==TC_HANDLE_OK,"board C unavailable");
 require(!handles.leaveBoardOnSceneChange(30),"the board-scene switch in the level's own frame invalidated the fresh handle");
 require(handles.valid(&third)==1,"the handle was refused inside its own level");
 require(handles.resolve(&third,&resolved)==TC_HANDLE_OK&&resolved==&boardC,"the live handle stopped resolving");
 require(handles.leaveBoardOnSceneChange(90),"a later scene change kept the handle");
 require(handles.valid(&third)==0,"the handle survived leaving the level");
 require(handles.current(TC_GAME_OBJECT_BOARD,&third)==TC_HANDLE_ERR_UNAVAILABLE,"the registry still has a board after leaving");
 std::cout<<"PASS game handles: issue, resolve, type guard, generation invalidation, board-scene entry and scene leave\n";
}
