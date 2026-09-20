/* Development driver: enter a board and then hold the mouse over a plugin's
   tool tile, so its hover expansion is open when the screenshot is taken.

   Why in-process: the sandbox window is parked off the desktop and its
   framebuffer cannot be read from outside, so the game itself has to press the
   home page entry and move the cursor (this build reads the cursor position
   itself, so SetCursorPos - not a synthetic WM_MOUSEMOVE - is what moves
   ImGui's mouse).

   The point to hover is the palette's tile slot in game coordinates, which is
   read from TC_TOOLBAR_POINT ("x y", default the measured 406 708 = the centre
   of the first plugin tile under the game's own tools). */
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <string>

struct V2 {float x,y;};

static const TCHost* host=nullptr;
static bool (*originalInvisible)(const char*,V2,int)=nullptr;
static HWND gameWindow=nullptr;
static bool entered=false;
static unsigned long long startTick=0;
static float pointX=406.f,pointY=708.f;
static bool logged=false;
static unsigned long long leaveAfter=0,firstHover=0;
static bool left=false;

static void log(const std::string& message) {
    if (host&&host->log) host->log(host->context,message.c_str());
}

static bool hookInvisible(const char* id,V2 size,int flags) {
    const bool result=originalInvisible?originalInvisible(id,size,flags):false;
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    if (!entered&&rva>=0x449df0&&rva<0x44b610) {
        if (!startTick) startTick=GetTickCount64();
        static int frame=-1,count=0;
        const auto getFrame=reinterpret_cast<int(*)()>(
            host->engine_proc(host->context,"igGetFrameCount"));
        const int current=getFrame?getFrame():0;
        if (current!=frame) {frame=current;count=0;}
        if (GetTickCount64()-startTick>4000&&++count==2) {
            entered=true;
            log("TOOLHOVER: pressed a home page entry to reach a board");
            return true;
        }
    }
    return result;
}

static BOOL CALLBACK windowCallback(HWND candidate,LPARAM data) {
    DWORD owner=0;
    GetWindowThreadProcessId(candidate,&owner);
    if (owner!=GetCurrentProcessId()) return TRUE;
    RECT rect{};
    if (!GetClientRect(candidate,&rect)) return TRUE;
    if (rect.right<320||rect.bottom<240) return TRUE;
    *reinterpret_cast<HWND*>(data)=candidate;
    return FALSE;
}

static void readPoint() {
    char buffer[128]{};
    const DWORD length=GetEnvironmentVariableA("TC_TOOLBAR_POINT",buffer,sizeof(buffer));
    if (length>0&&length<sizeof(buffer)) {
        float x=0.f,y=0.f;
        if (std::sscanf(buffer,"%f %f",&x,&y)==2&&x>0.f&&y>0.f) {pointX=x;pointY=y;}
    }
    char leave[32]{};
    if (GetEnvironmentVariableA("TC_TOOLHOVER_LEAVE",leave,sizeof(leave))>0)
        leaveAfter=(unsigned long long)std::atoi(leave);
}

static void frame(void*,const TCFrame* tick) {
    if (!entered) return;
    if (!gameWindow) {
        EnumWindows(windowCallback,reinterpret_cast<LPARAM>(&gameWindow));
        if (gameWindow) {
            /* Park it at the desktop origin: the game derives its mouse position
               from the real cursor, and screen/client/game coordinates only line
               up there. */
            SetWindowPos(gameWindow,nullptr,0,0,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            SetForegroundWindow(gameWindow);
        }
        return;
    }
    RECT client{};
    if (!GetClientRect(gameWindow,&client)||client.right<=0) return;
    /* The game's surface, not the current ImGui window: on_frame runs at the end
       of a frame with "Debug##Default" current, whose size is a meaningless 400
       and made an earlier version move the cursor to 2598,2832.  The main
       viewport is the surface the game actually draws and reads the mouse in. */
    const auto getViewport=reinterpret_cast<void*(*)()>(host->engine_proc(host->context,"igGetMainViewport"));
    void* viewport=getViewport?getViewport():nullptr;
    const float* data=viewport?static_cast<const float*>(viewport):nullptr;
    const float gameWidth=data?data[4]:0.f,gameHeight=data?data[5]:0.f;
    if (gameWidth<=0.f||gameHeight<=0.f) return;
    const int clientX=static_cast<int>(pointX*client.right/gameWidth);
    const int clientY=static_cast<int>(pointY*client.bottom/gameHeight);
    POINT screen{clientX,clientY};
    ClientToScreen(gameWindow,&screen);
    /* Optional: after holding the tile for a while, take the cursor away so a
       later screenshot shows the expanded UI closing again. */
    if (leaveAfter&&!left) {
        if (!firstHover) firstHover=GetTickCount64();
        else if (GetTickCount64()-firstHover>=leaveAfter) {
            POINT away{client.right-180,client.bottom-120};
            ClientToScreen(gameWindow,&away);
            /* Alternate by one pixel: Windows only queues WM_MOUSEMOVE when the
               position actually changes, so a constant SetCursorPos stops
               producing moves and a real user's mouse takes over again. */
            static bool toggle=false;
            toggle=!toggle;
            screen=away;
            if (toggle) screen.x+=1;
            left=true;
            log("TOOLHOVER: moved the cursor away from the tile after "+
                std::to_string((int)leaveAfter)+" ms");
        }
    }
    else if (left) {
        /* Keep issuing moves, one pixel apart, while staying off the tile. */
        static bool jiggle=false;
        jiggle=!jiggle;
        if (jiggle) screen.x+=1;
    }
    SetCursorPos(screen.x,screen.y);
    if (!logged&&tick&&tick->frame_number>0) {
        logged=true;
        log("TOOLHOVER: holding the cursor over the tool tile at game "+
            std::to_string((int)pointX)+","+std::to_string((int)pointY)+" -> client "+
            std::to_string(clientX)+","+std::to_string(clientY)+" (client "+
            std::to_string((int)client.right)+"x"+std::to_string((int)client.bottom)+
            ", game "+std::to_string((int)gameWidth)+"x"+std::to_string((int)gameHeight)+")");
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if (!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    readPoint();
    if (!h->create_hook) return 2;
    auto* target=h->resolve_symbol(h->context,"igInvisibleButton");
    if (!target) return 3;
    if (h->create_hook(h->context,target,reinterpret_cast<void*>(hookInvisible),
                       reinterpret_cast<void**>(&originalInvisible))!=0) return 4;
    out->on_frame=frame;
    log("TOOLHOVER: armed");
    return 0;
}
