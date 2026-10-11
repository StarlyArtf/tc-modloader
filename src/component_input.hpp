#pragma once

#include "../sdk/tc_service_api.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tc::component_input {

struct InstanceKey {
    uint64_t customId=0,instanceId=0,generation=0;
    bool operator<(const InstanceKey& other) const {
        if(customId!=other.customId)return customId<other.customId;
        if(instanceId!=other.instanceId)return instanceId<other.instanceId;
        return generation<other.generation;
    }
    bool operator==(const InstanceKey& other) const {
        return customId==other.customId&&instanceId==other.instanceId&&generation==other.generation;
    }
};
inline InstanceKey key(const TCComponentInstanceHandle& value){
    return {value.custom_id,value.instance_id,value.generation};
}

struct Handler {
    const void* owner=nullptr;
    float minX=0,minY=0,maxX=0,maxY=0;
    TCComponentInputCallbackV1 callback=nullptr;
    void* user=nullptr;
};

struct Target {
    TCComponentInstanceHandle handle{};
    float originX=0,originY=0;
    float axisXX=0,axisXY=0,axisYX=0,axisYY=0;
    float boardX=0,boardY=0;
    float boardAxisXX=1,boardAxisXY=0,boardAxisYX=0,boardAxisYY=1;
    uint64_t order=0;
};

struct PointerSample {
    float screenX=0,screenY=0,wheelX=0,wheelY=0;
    bool down=false,pressed=false,released=false,doubleClicked=false;
    uint32_t modifiers=0;
};

class Router {
    std::map<uint64_t,Handler> handlers;
    std::vector<Target> targets;
    InstanceKey hovered{},captured{},focused{},pressedTarget{};
    bool hasHovered=false,hasCaptured=false,hasFocused=false,hasPressed=false,dragging=false;
    float lastX=0,lastY=0,pressX=0,pressY=0;
    bool haveLast=false;
    uint64_t sequence=0;

    const Target* findTarget(const InstanceKey& wanted) const {
        const auto it=std::find_if(targets.begin(),targets.end(),[&](const Target& value){
            return key(value.handle)==wanted;
        });
        return it==targets.end()?nullptr:&*it;
    }
    const Handler* findHandler(uint64_t customId) const {
        const auto it=handlers.find(customId);return it==handlers.end()?nullptr:&it->second;
    }
    static bool local(const Target& target,float x,float y,float& localX,float& localY){
        const float dx=x-target.originX,dy=y-target.originY;
        const float det=target.axisXX*target.axisYY-target.axisXY*target.axisYX;
        if(!std::isfinite(det)||std::fabs(det)<1.0e-6f)return false;
        localX=(dx*target.axisYY-dy*target.axisYX)/det;
        localY=(-dx*target.axisXY+dy*target.axisXX)/det;
        return std::isfinite(localX)&&std::isfinite(localY);
    }
    const Target* hit(float x,float y,float* localX=nullptr,float* localY=nullptr) const {
        const Target* best=nullptr;float bx=0,by=0;
        for(const Target& target:targets){
            const Handler* handler=findHandler(target.handle.custom_id);
            if(!handler||!handler->callback)continue;
            float lx=0,ly=0;if(!local(target,x,y,lx,ly))continue;
            if(lx<handler->minX||lx>handler->maxX||ly<handler->minY||ly>handler->maxY)continue;
            if(!best||target.order>=best->order){best=&target;bx=lx;by=ly;}
        }
        if(best){if(localX)*localX=bx;if(localY)*localY=by;}return best;
    }
    uint32_t emit(const Target& target,uint32_t kind,float sx,float sy,float dx,float dy,
                  float wx,float wy,uint32_t button,uint32_t keyCode,uint32_t scancode,
                  uint32_t modifiers,uint32_t flags,uint32_t codepoint,
                  const char* text,uint32_t textBytes){
        const Handler* handler=findHandler(target.handle.custom_id);
        if(!handler||!handler->callback)return 0;
        TCComponentInputEventV1 event{};event.size=sizeof(event);event.version=1;
        event.kind=kind;event.flags=flags;event.sequence=++sequence;event.instance=target.handle;
        event.screen_x=sx;event.screen_y=sy;event.delta_x=dx;event.delta_y=dy;
        event.wheel_x=wx;event.wheel_y=wy;event.button=button;event.key=keyCode;
        event.scancode=scancode;event.modifiers=modifiers;event.codepoint=codepoint;
        float lx=0,ly=0;if(local(target,sx,sy,lx,ly)){event.local_x=lx;event.local_y=ly;}
        /* Board-space coordinates are reconstructed from the target's board
           centre and its rotated local axes. */
        event.board_x=target.boardX+lx*target.boardAxisXX+ly*target.boardAxisYX;
        event.board_y=target.boardY+lx*target.boardAxisXY+ly*target.boardAxisYY;
        event.text_size=std::min<uint32_t>(textBytes,sizeof(event.text)-1);
        if(text&&event.text_size)std::memcpy(event.text,text,event.text_size);
        uint32_t result=0;try{result=handler->callback(handler->user,&event);}catch(...){return 0;}
        if(kind!=TC_COMPONENT_INPUT_FOCUS_GAINED&&kind!=TC_COMPONENT_INPUT_FOCUS_LOST&&
           kind!=TC_COMPONENT_INPUT_CAPTURE_LOST)apply(target,result,sx,sy,modifiers);
        return result;
    }
    void simple(const InstanceKey& wanted,uint32_t kind){
        if(const Target* target=findTarget(wanted))emit(*target,kind,lastX,lastY,0,0,0,0,
                                                       UINT32_MAX,0,0,0,0,0,nullptr,0);
    }
    void setFocus(const Target* target){
        const InstanceKey next=target?key(target->handle):InstanceKey{};
        if(hasFocused&&(!target||!(focused==next)))simple(focused,TC_COMPONENT_INPUT_FOCUS_LOST);
        hasFocused=target!=nullptr;if(target)focused=next;
        if(target)simple(next,TC_COMPONENT_INPUT_FOCUS_GAINED);
    }
    void setCapture(const Target* target){
        if(hasCaptured&&(!target||!(captured==key(target->handle))))
            simple(captured,TC_COMPONENT_INPUT_CAPTURE_LOST);
        hasCaptured=target!=nullptr;if(target)captured=key(target->handle);
    }
    void apply(const Target& target,uint32_t result,float,float,uint32_t){
        if(result&TC_COMPONENT_INPUT_RESULT_RELEASE_POINTER)setCapture(nullptr);
        if(result&TC_COMPONENT_INPUT_RESULT_CAPTURE_POINTER)setCapture(&target);
        if(result&TC_COMPONENT_INPUT_RESULT_RELEASE_KEYBOARD)setFocus(nullptr);
        if(result&TC_COMPONENT_INPUT_RESULT_FOCUS_KEYBOARD){
            if(!hasFocused||!(focused==key(target.handle)))setFocus(&target);
        }
    }

public:
    int setHandler(const void* owner,uint64_t customId,float minX,float minY,float maxX,float maxY,
                   TCComponentInputCallbackV1 callback,void* user){
        if(!owner||!customId||!callback||!std::isfinite(minX)||!std::isfinite(minY)||
           !std::isfinite(maxX)||!std::isfinite(maxY)||!(maxX>minX)||!(maxY>minY))
            return TC_COMPONENT_INPUT_ERR_ARGUMENT;
        handlers[customId]={owner,minX,minY,maxX,maxY,callback,user};return TC_COMPONENT_INPUT_OK;
    }
    int clearHandler(const void* owner,uint64_t customId){
        const auto it=handlers.find(customId);
        if(it==handlers.end())return TC_COMPONENT_INPUT_ERR_STATE;
        if(it->second.owner!=owner)return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
        handlers.erase(it);return TC_COMPONENT_INPUT_OK;
    }
    void setTargets(std::vector<Target> next){
        auto exists=[&](const InstanceKey& wanted){
            return std::any_of(next.begin(),next.end(),[&](const Target& value){return key(value.handle)==wanted;});
        };
        auto prune=[&](bool& present,InstanceKey& value,uint32_t lost){
            if(present&&!exists(value)){if(lost)simple(value,lost);present=false;}
        };
        prune(hasHovered,hovered,TC_COMPONENT_INPUT_POINTER_LEAVE);
        prune(hasCaptured,captured,TC_COMPONENT_INPUT_CAPTURE_LOST);
        prune(hasFocused,focused,TC_COMPONENT_INPUT_FOCUS_LOST);
        prune(hasPressed,pressedTarget,0);targets=std::move(next);
    }
    bool pointer(const PointerSample& sample){
        const float dx=haveLast?sample.screenX-lastX:0,dy=haveLast?sample.screenY-lastY:0;
        lastX=sample.screenX;lastY=sample.screenY;haveLast=true;
        float lx=0,ly=0;const Target* under=hit(sample.screenX,sample.screenY,&lx,&ly);
        const Target* recipient=hasCaptured?findTarget(captured):under;
        const InstanceKey next=under?key(under->handle):InstanceKey{};
        bool consumed=false;
        if(hasHovered&&(!under||!(hovered==next))){
            if(const Target* old=findTarget(hovered))consumed|=(emit(*old,TC_COMPONENT_INPUT_POINTER_LEAVE,
                sample.screenX,sample.screenY,dx,dy,0,0,UINT32_MAX,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
            hasHovered=false;
        }
        if(under&&(!hasHovered||!(hovered==next))){hovered=next;hasHovered=true;
            consumed|=(emit(*under,TC_COMPONENT_INPUT_POINTER_ENTER,sample.screenX,sample.screenY,dx,dy,
                            0,0,UINT32_MAX,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;}
        if(recipient&&(dx!=0||dy!=0))consumed|=(emit(*recipient,TC_COMPONENT_INPUT_POINTER_MOVE,
            sample.screenX,sample.screenY,dx,dy,0,0,UINT32_MAX,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
        if(recipient&&(sample.wheelX!=0||sample.wheelY!=0))consumed|=(emit(*recipient,
            TC_COMPONENT_INPUT_POINTER_WHEEL,sample.screenX,sample.screenY,0,0,sample.wheelX,sample.wheelY,
            UINT32_MAX,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
        if(recipient&&sample.pressed){pressedTarget=key(recipient->handle);hasPressed=true;dragging=false;
            pressX=sample.screenX;pressY=sample.screenY;
            consumed|=(emit(*recipient,TC_COMPONENT_INPUT_POINTER_DOWN,sample.screenX,sample.screenY,0,0,
                            0,0,0,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;}
        if(recipient&&sample.doubleClicked)consumed|=(emit(*recipient,TC_COMPONENT_INPUT_POINTER_DOUBLE_CLICK,
            sample.screenX,sample.screenY,0,0,0,0,0,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
        if(sample.down&&hasPressed){
            if(!dragging&&std::hypot(sample.screenX-pressX,sample.screenY-pressY)>=4.f){dragging=true;
                if(const Target* down=findTarget(pressedTarget))consumed|=(emit(*down,TC_COMPONENT_INPUT_DRAG_BEGIN,
                    sample.screenX,sample.screenY,dx,dy,0,0,0,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;}
            else if(dragging)if(const Target* down=findTarget(pressedTarget))consumed|=(emit(*down,
                TC_COMPONENT_INPUT_DRAG_MOVE,sample.screenX,sample.screenY,dx,dy,0,0,0,0,0,
                sample.modifiers,0,0,nullptr,0)&1)!=0;
        }
        if(sample.released&&hasPressed){
            if(const Target* down=findTarget(pressedTarget)){
                consumed|=(emit(*down,TC_COMPONENT_INPUT_POINTER_UP,sample.screenX,sample.screenY,0,0,
                                0,0,0,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
                if(dragging)consumed|=(emit(*down,TC_COMPONENT_INPUT_DRAG_END,sample.screenX,sample.screenY,
                    0,0,0,0,0,0,0,sample.modifiers,0,0,nullptr,0)&1)!=0;
                else if(under&&key(under->handle)==pressedTarget)consumed|=(emit(*down,
                    TC_COMPONENT_INPUT_POINTER_CLICK,sample.screenX,sample.screenY,0,0,0,0,0,0,0,
                    sample.modifiers,0,0,nullptr,0)&1)!=0;
            }
            hasPressed=false;dragging=false;if(hasCaptured)setCapture(nullptr);
        }
        return consumed;
    }
    bool keyboard(uint32_t kind,uint32_t keyCode,uint32_t scancode,uint32_t modifiers,bool repeat){
        if(!hasFocused)return false;
        const Target* target=findTarget(focused);if(!target)return false;
        return (emit(*target,kind,lastX,lastY,0,0,0,0,UINT32_MAX,keyCode,scancode,modifiers,
                     repeat?TC_COMPONENT_INPUT_EVENT_REPEAT:0,0,nullptr,0)&1)!=0;
    }
    bool text(uint32_t kind,const char* utf8,uint32_t bytes,uint32_t codepoint=0){
        if(!hasFocused)return false;
        const Target* target=findTarget(focused);if(!target)return false;
        return (emit(*target,kind,lastX,lastY,0,0,0,0,UINT32_MAX,0,0,0,0,codepoint,
                     utf8,bytes)&1)!=0;
    }
    int route(const void* owner,const TCComponentInstanceHandle& handle,uint32_t requests){
        const Target* target=findTarget(key(handle));if(!target)return TC_COMPONENT_INPUT_ERR_STALE;
        const Handler* handler=findHandler(handle.custom_id);
        if(!handler||handler->owner!=owner)return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
        const uint32_t accepted=TC_COMPONENT_INPUT_RESULT_CAPTURE_POINTER|
            TC_COMPONENT_INPUT_RESULT_RELEASE_POINTER|TC_COMPONENT_INPUT_RESULT_FOCUS_KEYBOARD|
            TC_COMPONENT_INPUT_RESULT_RELEASE_KEYBOARD;
        if(requests&~accepted)return TC_COMPONENT_INPUT_ERR_ARGUMENT;
        apply(*target,requests,lastX,lastY,0);return TC_COMPONENT_INPUT_OK;
    }
    int state(const void* owner,const TCComponentInstanceHandle& handle,TCComponentInputStateV1* out) const {
        if(!out||out->size<sizeof(*out))return TC_COMPONENT_INPUT_ERR_ARGUMENT;
        const Handler* handler=findHandler(handle.custom_id);
        if(!handler||handler->owner!=owner)return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
        if(!findTarget(key(handle)))return TC_COMPONENT_INPUT_ERR_STALE;
        TCComponentInputStateV1 value{};value.size=sizeof(value);value.version=1;value.instance=handle;
        const InstanceKey wanted=key(handle);
        if(hasHovered&&hovered==wanted)value.flags|=TC_COMPONENT_INPUT_STATE_HOVERED;
        if(hasCaptured&&captured==wanted)value.flags|=TC_COMPONENT_INPUT_STATE_POINTER_CAPTURED;
        if(hasFocused&&focused==wanted)value.flags|=TC_COMPONENT_INPUT_STATE_KEYBOARD_FOCUSED;
        *out=value;return TC_COMPONENT_INPUT_OK;
    }
    void clearOwner(const void* owner){
        std::set<uint64_t> removed;
        for(auto it=handlers.begin();it!=handlers.end();)if(it->second.owner==owner){removed.insert(it->first);it=handlers.erase(it);}else ++it;
        targets.erase(std::remove_if(targets.begin(),targets.end(),[&](const Target& target){
            return removed.count(target.handle.custom_id)!=0;
        }),targets.end());
        if(hasHovered&&removed.count(hovered.customId))hasHovered=false;
        if(hasCaptured&&removed.count(captured.customId))hasCaptured=false;
        if(hasFocused&&removed.count(focused.customId))hasFocused=false;
        if(hasPressed&&removed.count(pressedTarget.customId))hasPressed=false;
    }
    void clearBoard(){
        if(hasHovered)simple(hovered,TC_COMPONENT_INPUT_POINTER_LEAVE);
        if(hasCaptured)simple(captured,TC_COMPONENT_INPUT_CAPTURE_LOST);
        if(hasFocused)simple(focused,TC_COMPONENT_INPUT_FOCUS_LOST);
        targets.clear();hasHovered=hasCaptured=hasFocused=hasPressed=dragging=false;
    }
    bool hasHandlers() const{return !handlers.empty();}
    bool keyboardFocused() const{return hasFocused;}
};

} // namespace tc::component_input
