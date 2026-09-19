/* Development driver: hold the real cursor at a fixed game position.

   The game's bottom drawer shows the component under the cursor, so pinning the
   cursor onto a placed component is what makes the drawer show that component's
   preview (and therefore its pin names).  This build reads the cursor position
   itself, so SetCursorPos is what moves ImGui's mouse; the window is parked at
   the desktop origin for the run so screen, client and game coordinates line up.

   TC_CURSOR_POINT="x y"  game coordinates (default 1250 430)
   TC_CURSOR_DELAY=<ms>   wait before pinning (default 0)
   TC_CURSOR_CLICK=1      click once at that position (selects what is under it) */
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <string>

static const TCHost* host=nullptr;
static HWND gameWindow=nullptr;
static float pointX=1250.f,pointY=430.f;
static float point2X=0.f,point2Y=0.f;
static unsigned long long secondDelayMs=0;
static unsigned long long delayMs=0,startTick=0;
static bool pinned=false;

static void log(const std::string& message) {
    if (host&&host->log) host->log(host->context,message.c_str());
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

static void readConfiguration() {
    char buffer[128]{};
    if (GetEnvironmentVariableA("TC_CURSOR_POINT",buffer,sizeof(buffer))>0) {
        float x=0.f,y=0.f;
        if (std::sscanf(buffer,"%f %f",&x,&y)==2&&x>0.f&&y>0.f) {pointX=x;pointY=y;}
    }
    char delay[32]{};
    if (GetEnvironmentVariableA("TC_CURSOR_DELAY",delay,sizeof(delay))>0)
        delayMs=(unsigned long long)std::atoi(delay);
    char second[128]{};
    if (GetEnvironmentVariableA("TC_CURSOR_POINT2",second,sizeof(second))>0) {
        float x=0.f,y=0.f;
        if (std::sscanf(second,"%f %f",&x,&y)==2&&x>0.f&&y>0.f) {point2X=x;point2Y=y;}
    }
    char secondDelay[32]{};
    if (GetEnvironmentVariableA("TC_CURSOR_CLICK2_DELAY",secondDelay,sizeof(secondDelay))>0)
        secondDelayMs=(unsigned long long)std::atoi(secondDelay);
}

static void frame(void*,const TCFrame* tick) {
    if (!gameWindow) {
        EnumWindows(windowCallback,reinterpret_cast<LPARAM>(&gameWindow));
        if (gameWindow) {
            SetWindowPos(gameWindow,nullptr,0,0,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            SetForegroundWindow(gameWindow);
        }
        return;
    }
    if (!startTick) startTick=GetTickCount64();
    if (GetTickCount64()-startTick<delayMs) return;
    RECT client{};
    if (!GetClientRect(gameWindow,&client)||client.right<=0) return;
    const auto getViewport=reinterpret_cast<void*(*)()>(
        host->engine_proc(host->context,"igGetMainViewport"));
    void* viewport=getViewport?getViewport():nullptr;
    const float* data=viewport?static_cast<const float*>(viewport):nullptr;
    const float gameWidth=data?data[4]:0.f,gameHeight=data?data[5]:0.f;
    if (gameWidth<=0.f||gameHeight<=0.f) return;
    /* Two-stage pointing: some panels have to be opened first and then an entry
       inside them selected (the component menu's category tab, then the
       thumbnail); point2 switches the target after the first click. */
    static bool movedToSecond=false;
    if (secondDelayMs&&GetTickCount64()-startTick>=secondDelayMs) movedToSecond=true;
    const float targetX=movedToSecond&&point2X>0.f?point2X:pointX;
    const float targetY=movedToSecond&&point2Y>0.f?point2Y:pointY;
    const int clientX=static_cast<int>(targetX*client.right/gameWidth);
    const int clientY=static_cast<int>(targetY*client.bottom/gameHeight);
    POINT screen{clientX,clientY};
    ClientToScreen(gameWindow,&screen);
    /* Nudge by a pixel when the position repeats: Windows only queues a move when
       the position changes, and a real hand on the mouse would otherwise win. */
    static bool toggle=false;
    toggle=!toggle;
    if (toggle) ++screen.x;
    SetCursorPos(screen.x,screen.y);
    /* The drawer's preview follows the *selected* component, not just the
       hovered one, so the driver can click once the cursor is in place. */
    static bool clicked=false;
    static bool clicked2=false;
    const unsigned long long now=GetTickCount64()-startTick;
    if (!clicked&&GetEnvironmentVariableA("TC_CURSOR_CLICK",nullptr,0)>0&&
        now>delayMs+1500&&!movedToSecond) {
        clicked=true;
        const LPARAM point=MAKELPARAM(clientX,clientY);
        const bool right=GetEnvironmentVariableA("TC_CURSOR_RIGHTCLICK",nullptr,0)>0;
        PostMessageW(gameWindow,right?WM_RBUTTONDOWN:WM_LBUTTONDOWN,right?MK_RBUTTON:MK_LBUTTON,point);
        Sleep(60);
        PostMessageW(gameWindow,right?WM_RBUTTONUP:WM_LBUTTONUP,0,point);
        log("CURSOR: clicked at client "+std::to_string(clientX)+","+std::to_string(clientY));
    }
    if (clicked&&!clicked2&&secondDelayMs&&movedToSecond&&now>delayMs+3000) {
        clicked2=true;
        const LPARAM point=MAKELPARAM(clientX,clientY);
        PostMessageW(gameWindow,WM_LBUTTONDOWN,MK_LBUTTON,point);
        Sleep(60);
        PostMessageW(gameWindow,WM_LBUTTONUP,0,point);
        log("CURSOR: second click at game "+std::to_string((int)targetX)+","+
            std::to_string((int)targetY)+" -> client "+std::to_string(clientX)+","+std::to_string(clientY));
    }
    if (!pinned) {
        pinned=true;
        log("CURSOR: pinned at game "+std::to_string((int)pointX)+","+
            std::to_string((int)pointY)+" -> client "+std::to_string(clientX)+","+
            std::to_string(clientY)+" frame="+std::to_string(tick?tick->frame_number:-1));
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if (!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    readConfiguration();
    out->on_frame=frame;
    log("CURSOR: armed");
    return 0;
}
