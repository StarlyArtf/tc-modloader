#include "../src/component_input.hpp"
#include "../sdk/tc_component_input.h"
#include <cassert>
#include <iostream>
#include <vector>

struct Seen {std::vector<uint32_t> kinds;std::vector<uint64_t> instances;};
static uint32_t callback(void* user,const TCComponentInputEventV1* event){
    auto& seen=*static_cast<Seen*>(user);seen.kinds.push_back(event->kind);
    seen.instances.push_back(event->instance.instance_id);
    if(event->kind==TC_COMPONENT_INPUT_POINTER_DOWN)
        return TC_COMPONENT_INPUT_RESULT_CONSUMED|TC_COMPONENT_INPUT_RESULT_CAPTURE_POINTER|
               TC_COMPONENT_INPUT_RESULT_FOCUS_KEYBOARD;
    return TC_COMPONENT_INPUT_RESULT_CONSUMED;
}
static bool has(const Seen& seen,uint32_t kind){
    return std::find(seen.kinds.begin(),seen.kinds.end(),kind)!=seen.kinds.end();
}
int main(){
    tc::component_input::Router router;int owner=0;Seen seen;
    assert(router.setHandler(&owner,9,-1,-0.5f,1,0.5f,callback,&seen)==0);
    TCComponentInstanceHandle handle{sizeof(handle),1,9,21,4};
    tc::component_input::Target target{};target.handle=handle;target.originX=100;target.originY=50;
    /* 90-degree rotated, 10 pixels per local unit. */
    target.axisXX=0;target.axisXY=10;target.axisYX=-10;target.axisYY=0;
    target.boardX=4;target.boardY=5;router.setTargets({target});
    tc::component_input::PointerSample sample{};sample.screenX=100;sample.screenY=55;
    assert(router.pointer(sample));assert(has(seen,TC_COMPONENT_INPUT_POINTER_ENTER));
    sample.pressed=true;sample.down=true;assert(router.pointer(sample));
    TCComponentInputStateV1 state{};state.size=sizeof(state);
    assert(router.state(&owner,handle,&state)==0);
    assert((state.flags&TC_COMPONENT_INPUT_STATE_POINTER_CAPTURED)!=0);
    assert((state.flags&TC_COMPONENT_INPUT_STATE_KEYBOARD_FOCUSED)!=0);
    sample.pressed=false;sample.screenX=120;sample.screenY=70;
    assert(router.pointer(sample));assert(has(seen,TC_COMPONENT_INPUT_DRAG_BEGIN));
    sample.down=false;sample.released=true;assert(router.pointer(sample));
    assert(has(seen,TC_COMPONENT_INPUT_DRAG_END));
    assert(router.keyboard(TC_COMPONENT_INPUT_KEY_DOWN,546,30,TC_COMPONENT_INPUT_MOD_CTRL,false));
    assert(router.text(TC_COMPONENT_INPUT_TEXT_COMMIT,"\xe4\xb8\xad",3,0x4e2d));
    assert(router.text(TC_COMPONENT_INPUT_IME_BEGIN,"",0));
    assert(router.text(TC_COMPONENT_INPUT_IME_UPDATE,"zhong",5));
    assert(router.text(TC_COMPONENT_INPUT_IME_END,"",0));
    assert(has(seen,TC_COMPONENT_INPUT_KEY_DOWN)&&has(seen,TC_COMPONENT_INPUT_TEXT_COMMIT)&&
           has(seen,TC_COMPONENT_INPUT_IME_BEGIN)&&has(seen,TC_COMPONENT_INPUT_IME_UPDATE)&&
           has(seen,TC_COMPONENT_INPUT_IME_END));
    TCComponentInstanceHandle secondHandle{sizeof(secondHandle),1,9,22,1};
    tc::component_input::Target second=target;second.handle=secondHandle;second.originX=200;second.order=2;
    router.setTargets({target,second});
    sample={};sample.screenX=200;sample.screenY=55;router.pointer(sample);
    sample.pressed=true;sample.down=true;assert(router.pointer(sample));
    assert(router.state(&owner,secondHandle,&state)==0&&
           (state.flags&TC_COMPONENT_INPUT_STATE_KEYBOARD_FOCUSED));
    assert(router.state(&owner,handle,&state)==0&&
           !(state.flags&TC_COMPONENT_INPUT_STATE_KEYBOARD_FOCUSED));
    assert(has(seen,TC_COMPONENT_INPUT_FOCUS_LOST));
    router.setTargets({});
    assert(router.state(&owner,handle,&state)==TC_COMPONENT_INPUT_ERR_STALE);
    router.clearOwner(&owner);assert(!router.hasHandlers());
    std::cout<<"component-input tests passed\n";
}
