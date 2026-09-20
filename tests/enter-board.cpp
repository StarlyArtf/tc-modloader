/* Development probe: enter a board (level) without any input, so a screenshot
   run can look at things that only exist inside a level - board side panels,
   the wire palette, the circuit itself.

   It presses one of the home page's own invisible buttons, the same trick the
   board-panel and waveform drivers use, and hooks nothing else: the mods under
   test need `handle_update_wire` for themselves, so this probe must not take it. */
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <string>

struct TCEnterBoardV2 {float x,y;};

static const TCHost* host;
static bool (*originalInvisible)(const char*,TCEnterBoardV2,int)=nullptr;
static bool entered=false;
static unsigned long long startTick=0;

static void log(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static bool hookInvisible(const char* id,TCEnterBoardV2 size,int flags) {
    const bool result=originalInvisible?originalInvisible(id,size,flags):false;
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    if (!entered && rva>=0x449df0 && rva<0x44b610) {
        if (!startTick) startTick=GetTickCount64();
        /* Two presses a frame apart, a few seconds in: the same sequence the
           other drivers use to get from the home page into a board. */
        static int frame=-1,count=0;
        const auto getFrame=reinterpret_cast<int(*)()>(
            host->engine_proc(host->context,"igGetFrameCount"));
        const int current=getFrame?getFrame():0;
        if (current!=frame) { frame=current; count=0; }
        if (GetTickCount64()-startTick>4000 && ++count==2) {
            entered=true;
            log("ENTER-BOARD: pressed a home page entry to reach a board");
            return true;
        }
    }
    return result;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if (!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    if (!h->create_hook) return 2;
    auto* target=h->resolve_symbol(h->context,"igInvisibleButton");
    if (!target) return 3;
    if (h->create_hook(h->context,target,reinterpret_cast<void*>(hookInvisible),
                       reinterpret_cast<void**>(&originalInvisible))!=0) return 4;
    log("ENTER-BOARD: armed");
    return 0;
}
