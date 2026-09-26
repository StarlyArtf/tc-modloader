#pragma once
#include "../sdk/tc_handle_api.h"
#include <mutex>
#include <unordered_map>

namespace tc {
class GameHandles {
    mutable std::mutex lock;
    void* board=nullptr;
    uint64_t generation=1,token=0;
    struct Child {uint32_t kind;const void* value;};
    std::unordered_map<uint64_t,Child> children;
    uint64_t childGeneration=1,childToken=0;
    int64_t childFrame=-1;
    void invalidateChildren(){++childGeneration;children.clear();}
    /* The engine frame of the last level.load.  The pinned build loads the level
       and switches to the board scene in the *same* frame, so that switch is the
       entry, not a leave: invalidating there would kill the handle the level just
       issued and leave the whole level without one (measured by
       tests/game-handle-probe-playtest.ps1).  A switch in a later frame is a real
       leave, which is the case the registry exists for.  The frame is the
       discriminator rather than "is there a load behind it" because a mod can
       load another level while the board scene stays up, and that later switch
       still has to invalidate. */
    int64_t loadedFrame=-1;
public:
    void enterBoard(void* value,int64_t frame){std::lock_guard<std::mutex> guard(lock);++generation;++token;board=value;loadedFrame=frame;childFrame=frame;invalidateChildren();}
    /* Unconditional invalidation; used by the unit test and by anything that
       knows the board is gone. */
    void leaveBoard(){std::lock_guard<std::mutex> guard(lock);++generation;board=nullptr;loadedFrame=-1;childFrame=-1;invalidateChildren();}
    /* The scene.change detour calls this before the game's own function runs.
       Returns true when the switch really left the board, false when it belongs
       to the entry: the board's level was loaded in this very frame (or the one
       before, in case the game splits the flow across a frame boundary). */
    bool leaveBoardOnSceneChange(int64_t frame){
        std::lock_guard<std::mutex> guard(lock);
        if(board&&frame>=0&&loadedFrame>=0&&frame<=loadedFrame+1){loadedFrame=-1;return false;}
        ++generation;board=nullptr;loadedFrame=-1;childFrame=-1;invalidateChildren();return true;
    }
    /* Component/Wire handles are deliberately frame-scoped.  Their backing
       Nim sequences may reallocate when the player edits a board, so keeping a
       raw child pointer across a game frame would turn a valid-looking handle
       into a use-after-free. */
    void beginFrame(int64_t frame){
        std::lock_guard<std::mutex> guard(lock);
        if(frame!=childFrame){childFrame=frame;invalidateChildren();}
    }
    int beginChildSnapshot(const TCGameHandle* owner,int64_t frame,uint64_t* outGeneration){
        if(!owner||!outGeneration||owner->size<sizeof(TCGameHandle))return TC_HANDLE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> guard(lock);
        if(owner->kind!=TC_GAME_OBJECT_BOARD)return TC_HANDLE_ERR_KIND;
        if(!board||owner->generation!=generation||owner->token!=token)return TC_HANDLE_ERR_STALE;
        if(frame!=childFrame)return TC_HANDLE_ERR_STALE;
        *outGeneration=childGeneration;return TC_HANDLE_OK;
    }
    int issueChild(uint32_t kind,const void* value,uint64_t snapshotGeneration,TCGameHandle* out){
        if(!value||!out)return TC_HANDLE_ERR_ARGUMENT;
        if(kind!=TC_GAME_OBJECT_COMPONENT&&kind!=TC_GAME_OBJECT_WIRE)return TC_HANDLE_ERR_KIND;
        std::lock_guard<std::mutex> guard(lock);
        if(!board||snapshotGeneration!=childGeneration)return TC_HANDLE_ERR_STALE;
        const uint64_t issued=++childToken;
        children.emplace(issued,Child{kind,value});
        *out=TCGameHandle{sizeof(TCGameHandle),kind,childGeneration,issued};return TC_HANDLE_OK;
    }
    int current(uint32_t kind,TCGameHandle* out) const {
        if(!out)return TC_HANDLE_ERR_ARGUMENT;
        if(kind!=TC_GAME_OBJECT_BOARD)return TC_HANDLE_ERR_KIND;
        std::lock_guard<std::mutex> guard(lock);
        if(!board)return TC_HANDLE_ERR_UNAVAILABLE;
        *out=TCGameHandle{sizeof(TCGameHandle),kind,generation,token};return TC_HANDLE_OK;
    }
    int resolve(const TCGameHandle* handle,const void** out) const {
        if(out)*out=nullptr;
        if(!handle||!out||handle->size<sizeof(TCGameHandle))return TC_HANDLE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> guard(lock);
        if(handle->kind==TC_GAME_OBJECT_BOARD){
            if(!board||handle->generation!=generation||handle->token!=token)return TC_HANDLE_ERR_STALE;
            *out=board;return TC_HANDLE_OK;
        }
        if(handle->kind==TC_GAME_OBJECT_COMPONENT||handle->kind==TC_GAME_OBJECT_WIRE){
            auto found=children.find(handle->token);
            if(!board||handle->generation!=childGeneration||found==children.end()||found->second.kind!=handle->kind)return TC_HANDLE_ERR_STALE;
            *out=found->second.value;return TC_HANDLE_OK;
        }
        return TC_HANDLE_ERR_KIND;
    }
    int valid(const TCGameHandle* handle) const {const void* value=nullptr;int result=resolve(handle,&value);return result==TC_HANDLE_OK?1:(result==TC_HANDLE_ERR_STALE?0:result);}
};
}
