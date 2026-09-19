/* Development-only probe. Hooks the EXE thunk at the verified board input
   sampling callsite. Synthetic ActiveID injection tests timing, NOT real
   mouse clicks. Nothing here is a public plugin UI API. */
#include "../sdk/tc_ui_draw.h"
#include "../sdk/tc_game_model.h"
#include <stdexcept>
using namespace tc::ui;
static const TCHost* host;
static tc::TCGameModel game;
static bool (*activeOriginal)();
static bool (*invisibleOriginal)(const char*,Vec2,int);
static bool (*updateOriginal)(void*,void*,void*,uint32_t,uint8_t);
static void (*loadLevel)(void*,const tc::TCNimString*);
static void* (*currentWindow)();
static void (*setActive)(unsigned,void*);
static unsigned (*getId)(const char*);
static bool (*bgActive)();
static void* model;
static double started=-1,elapsed=0;
static bool loaded=false,failed=false;
static int boardFrames=0,lastBoardFrame=-1,menuFrame=-1,menuButton=0;
static void log(const std::string& s){host->log(host->context,s.c_str());}
static void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static bool activeHook(){
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    if(rva!=0x46b593 || !loaded || failed || boardFrames>=180)return activeOriginal();
    try {
        const int frame=frameCount();check(frame!=lastBoardFrame,"duplicate board sample");lastBoardFrame=frame;
        void* parent=currentWindow();const Vec2 cursor=cursorPos();
        const float width=windowWidth(),height=windowHeight();
        check(width>640&&height>400,"invalid board parent geometry");
        setCursorPos({width-360,96});
        const bool inject=boardFrames>=30&&boardFrames<60;
        const bool clear=boardFrames==60;
        if(clear)setActive(0,nullptr);
        {
            Child sidebar("TC board probe",{340,300},1);
            if(sidebar){
                Id modScope("dev.board-slot-probe");Id slotScope("sidebar");
                text("Board slot probe");text("draw="+std::to_string(boardFrames));
                const unsigned id=getId("capture");
                invisibleButton("capture",{250,45});
                if(inject)setActive(id,currentWindow());
                Canvas canvas("geometry",{270,120});check(bool(canvas),"canvas failed");
                canvas.rectFilled({0,0},{270,120},rgba(35,40,50));
                canvas.circleFilled({60,60},28,rgba(80,165,225));
                canvas.line({110,20},{230,100},rgba(245,185,70),3);
            }else check(false,"sidebar clipped unexpectedly");
        }
        check(currentWindow()==parent,"parent window not restored");
        setCursorPos(cursor);
        const bool active=activeOriginal();
        if(inject)check(active&&!bgActive(),"board input sample did not see active sidebar widget");
        if(clear)check(!active,"active state remained after clear");
        ++boardFrames;
        if(boardFrames==1)log("BOARD SLOT geometry parent="+std::to_string(int(width))+"x"+
            std::to_string(int(height))+" child=340x300 origin="+std::to_string(int(width-360))+",96");
        if(boardFrames==60)log("BOARD SLOT PASS synthetic active widget visible before native input sample (30 frames)");
        if(boardFrames==61)log("BOARD SLOT PASS inactive state restored");
        if(boardFrames==180)log("BOARD SLOT PASS frames=180 exact callsite, child/canvas, parent restoration");
        return active;
    }catch(const std::exception& e){failed=true;log(std::string("BOARD SLOT FAIL ")+e.what());return activeOriginal();}
}
static bool updateHook(void* m,void* c,void* input,uint32_t point,uint8_t flags){
    model=m;return updateOriginal(m,c,input,point,flags);
}
static bool invisibleHook(const char* id,Vec2 size,int flags){
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    const bool result=invisibleOriginal(id,size,flags);
    if(rva>=0x449df0&&rva<0x44b610&&!loaded){
        if(menuFrame!=frameCount()){menuFrame=frameCount();menuButton=0;}
        if(++menuButton==2&&elapsed>3)return true;
    }
    return result;
}
static void frame(void*,const TCFrame* f){
    if(started<0)started=f->time_seconds;elapsed=f->time_seconds-started;
    if(!loaded&&model&&elapsed>4){
        check(boardFrames==0,"sidebar drew before board entry");
        tc::TCNimString name{};const char* level="sandbox";
        game.raw_new_string(&name,7);name.length=7;
        std::memcpy((char*)name.data+8,level,8);loadLevel(model,&name);loaded=true;
        log("BOARD SLOT entered sandbox; no sidebar callbacks before entry");
    }
}
template<class T> static T engine(const char* name){return reinterpret_cast<T>(host->engine_proc(host->context,name));}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out){
    host=h;if(!load(h)||!loadDrawing(h)||!game.load(h))return 1;
    currentWindow=engine<decltype(currentWindow)>("igGetCurrentWindow");
    setActive=engine<decltype(setActive)>("igSetActiveID");getId=engine<decltype(getId)>("igGetID_Str");
    bgActive=engine<decltype(bgActive)>("igIsWindowBgActive");
    loadLevel=reinterpret_cast<decltype(loadLevel)>(h->resolve_symbol(h->context,"load_level__modelZutilities_u7740"));
    if(!currentWindow||!setActive||!getId||!bgActive||!loadLevel)return 2;
    if(h->create_hook(h->context,h->resolve_symbol(h->context,"igIsAnyItemActive"),
        reinterpret_cast<void*>(activeHook),reinterpret_cast<void**>(&activeOriginal)))return 3;
    if(h->create_hook(h->context,h->resolve_symbol(h->context,"igInvisibleButton"),
        reinterpret_cast<void*>(invisibleHook),reinterpret_cast<void**>(&invisibleOriginal)))return 4;
    if(h->create_hook(h->context,h->resolve_symbol(h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5"),
        reinterpret_cast<void*>(updateHook),reinterpret_cast<void**>(&updateOriginal)))return 5;
    out->on_frame=frame;return 0;
}
