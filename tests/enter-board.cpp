/* Development probe: enter a board (level) without any input, so a screenshot
   run can look at things that only exist inside a level - board side panels,
   the wire palette, the circuit itself.

   It presses one of the home page's own invisible buttons, the same trick the
   board-panel and waveform drivers use, and hooks nothing else: the mods under
   test need `handle_update_wire` for themselves, so this probe must not take it. */
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct TCEnterBoardV2 {float x,y;};

static const TCHost* host;
static bool (*originalInvisible)(const char*,TCEnterBoardV2,int)=nullptr;
static void (*cursorScreenPos)(TCEnterBoardV2*)=nullptr;
static bool entered=false;
static unsigned long long startTick=0;
static unsigned long long stageTick=0;
static int targets[4]={2,0,0,0};
static int targetCount=1;
static int stage=0;
static bool traceCandidates=false;
static int traceFrame=-1;
static bool traceDone=false;
static int followX[4]={-1,-1,-1,-1};
static int followY[4]={-1,-1,-1,-1};
static int followCount=0;
static int followIndex=0;
static int scrollNotches=0;
static bool scrollSent=false;
static int scrollX=150;
static int scrollY=500;

static void log(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static HWND gameWindow() {
    struct Finder { DWORD process; HWND found; } finder{GetCurrentProcessId(),nullptr};
    EnumWindows(
        [](HWND window,LPARAM data)->BOOL {
            auto& finder=*reinterpret_cast<Finder*>(data);
            DWORD owner=0;
            GetWindowThreadProcessId(window,&owner);
            if(owner!=finder.process||!IsWindowVisible(window)) return TRUE;
            RECT client{};
            if(!GetClientRect(window,&client)||client.right<320||client.bottom<240) return TRUE;
            finder.found=window;
            return FALSE;
        },reinterpret_cast<LPARAM>(&finder));
    return finder.found;
}

static void frame(void*,const TCFrame*) {
    if(!entered||GetTickCount64()-stageTick<2500) return;
    HWND window=gameWindow();
    if(!window) return;
    if(followIndex>=followCount) {
        if(scrollSent||scrollNotches==0) return;
        scrollSent=true;
        POINT point{scrollX,scrollY};
        ClientToScreen(window,&point);
        SetCursorPos(point.x,point.y);
        const short delta=static_cast<short>(scrollNotches*WHEEL_DELTA);
        PostMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(scrollX,scrollY));
        PostMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),
                      MAKELPARAM(point.x,point.y));
        log("ENTER-BOARD: scrolled left panel "+std::to_string(scrollNotches)+
            " notch(es) at ("+std::to_string(scrollX)+","+std::to_string(scrollY)+")");
        return;
    }
    const int x=followX[followIndex],y=followY[followIndex];
    ++followIndex;
    stageTick=GetTickCount64();
    PostMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
    PostMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    PostMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    log("ENTER-BOARD: clicked follow-up #"+std::to_string(followIndex)+" point ("+
        std::to_string(x)+","+std::to_string(y)+")");
}

static bool hookInvisible(const char* id,TCEnterBoardV2 size,int flags) {
    const bool result=originalInvisible?originalInvisible(id,size,flags):false;
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    const bool onHome=rva>=0x449df0 && rva<0x44b610;
    const bool inScope=onHome || (targetCount>1 && stage>0);
    /* The loader draws entries of its own on the home page (Mods, and since
       0.8.0 the 存档 page).  They are not part of the candidate numbering this
       driver's cases configure - counting them shifted every target by one and
       made the drivers press the wrong entry. */
    const bool loaderEntry=onHome&&id&&(std::strcmp(id,"Mods")==0||std::strcmp(id,"Saves")==0);
    if (!entered && inScope && !loaderEntry) {
        if (!startTick) startTick=GetTickCount64();
        if (!stageTick) stageTick=startTick;
        /* Two presses a frame apart, a few seconds in: the same sequence the
           other drivers use to get from the home page into a board. */
        static int frame=-1,count=0;
        const auto getFrame=reinterpret_cast<int(*)()>(
            host->engine_proc(host->context,"igGetFrameCount"));
        const int current=getFrame?getFrame():0;
        if (current!=frame) { frame=current; count=0; }
        ++count;
        const auto now=GetTickCount64();
        const auto elapsed=now-stageTick;
        if (traceCandidates && elapsed>(stage==0?2500:1200) && !traceDone) {
            if (traceFrame<0) traceFrame=current;
            if (current==traceFrame) {
                TCEnterBoardV2 pos{};
                if (cursorScreenPos) cursorScreenPos(&pos);
                char line[256];
                std::snprintf(line,sizeof(line),
                    "ENTER-BOARD: stage %d candidate #%d id=\"%s\" rva=0x%llx pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                    stage,count,id?id:"",static_cast<unsigned long long>(rva),
                    static_cast<double>(pos.x),static_cast<double>(pos.y),
                    static_cast<double>(size.x),static_cast<double>(size.y));
                log(line);
            } else {
                traceDone=true;
            }
        }
        const int target=stage<targetCount?targets[stage]:0;
        if (target>0 && elapsed>(stage==0?4000:2200) && count==target) {
            log("ENTER-BOARD: pressed stage "+std::to_string(stage)+" candidate #"+
                std::to_string(target)+" id=\""+(id?id:"")+"\"");
            ++stage;
            stageTick=now;
            traceFrame=-1;
            traceDone=false;
            entered=stage>=targetCount;
            return true;
        }
    }
    return result;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if (!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    char setting[32]{};
    if (GetEnvironmentVariableA("TC_ENTER_BOARD_SEQUENCE",setting,sizeof(setting))>0) {
        targetCount=std::sscanf(setting,"%d,%d,%d,%d",&targets[0],&targets[1],&targets[2],&targets[3]);
        if (targetCount<1) { targets[0]=2; targetCount=1; }
    } else if (GetEnvironmentVariableA("TC_ENTER_BOARD_INDEX",setting,sizeof(setting))>0) {
        targets[0]=std::atoi(setting);
        targetCount=1;
    }
    traceCandidates=GetEnvironmentVariableA("TC_ENTER_BOARD_LOG",setting,sizeof(setting))>0;
    if(GetEnvironmentVariableA("TC_ENTER_BOARD_POINTS",setting,sizeof(setting))>0) {
        followCount=std::sscanf(setting,"%d,%d;%d,%d;%d,%d;%d,%d",
            &followX[0],&followY[0],&followX[1],&followY[1],
            &followX[2],&followY[2],&followX[3],&followY[3])/2;
    } else if(GetEnvironmentVariableA("TC_ENTER_BOARD_POINT",setting,sizeof(setting))>0) {
        followCount=std::sscanf(setting,"%d,%d",&followX[0],&followY[0])/2;
    }
    if(GetEnvironmentVariableA("TC_ENTER_BOARD_SCROLL",setting,sizeof(setting))>0)
        scrollNotches=std::atoi(setting);
    if(GetEnvironmentVariableA("TC_ENTER_BOARD_SCROLL_POINT",setting,sizeof(setting))>0)
        std::sscanf(setting,"%d,%d",&scrollX,&scrollY);
    if (!h->create_hook) return 2;
    auto* target=h->resolve_symbol(h->context,"igInvisibleButton");
    if (!target) return 3;
    if (void* entry=h->engine_proc(h->context,"igGetCursorScreenPos"))
        std::memcpy(&cursorScreenPos,&entry,sizeof(entry));
    if (h->create_hook(h->context,target,reinterpret_cast<void*>(hookInvisible),
                       reinterpret_cast<void**>(&originalInvisible))!=0) return 4;
    std::string sequence;
    for(int i=0;i<targetCount;++i){if(i)sequence+=',';sequence+=std::to_string(targets[i]);}
    log("ENTER-BOARD: armed sequence="+sequence+
        (traceCandidates?" trace=1":" trace=0"));
    out->on_frame=&frame;
    return 0;
}
