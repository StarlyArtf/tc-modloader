/* M0.5 (C2 validation): make the loop's wire dangle **in the board model**, then
   let the game compile as usual.

   Why: with a cycle the compile produces an empty program (the ordered
   component list the emitter walks is empty), and turning the circular
   dependency annotation off does not change that (see the M0.5 experiment).
   C2 says: make the board acyclic for the compile, then re-close the connection
   in the generated program with our own delay semantics.  This probe checks the
   first half: after moving one wire end three cells away from its pin, does the
   generated program carry the component blocks again?

   Board wire table: board+0x98 length, board+0xa0 payload, element i at
   payload+8+i*0x68, both endpoints as int16 pairs at +0x18/+0x1a and
   +0x1c/+0x1e (src/board_objects.hpp). */
#include "../sdk/tc_hook.h"
#include "../sdk/tc_mod_api.h"
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const TCHost* host=nullptr;
void* boardModel=nullptr;
int framesSinceLoad=-1;
bool patched=false;
int cutFrames=40;

std::string say(const std::string& text){return "WIRE-CUT: "+text;}

int16_t readI16(const unsigned char* base,int offset){
    int16_t value=0; std::memcpy(&value,base+offset,sizeof(value)); return value;
}
void writeI16(unsigned char* base,int offset,int16_t value){
    std::memcpy(base+offset,&value,sizeof(value));
}
uint64_t readU64(const unsigned char* base,int offset){
    uint64_t value=0; std::memcpy(&value,base+offset,sizeof(value)); return value;
}
const unsigned char* readPtr(const unsigned char* base,int offset){
    const unsigned char* value=nullptr; std::memcpy(&value,base+offset,sizeof(value)); return value;
}

/* Move the wire end that sits on a pin far away, so the board the compiler sees
   is acyclic.  Logged with the before/after endpoints, because "did the patch
   land" has to be observable from the loader log. */
void cutNow(void){
    if(!boardModel) return;
    static int cuts=0;
    if(cuts>=8) return;   /* one per level load; several levels load in a run */
    ++cuts;
    /* Two modes, chosen by the playtest through TC_WIRE_PROBE:

         cut   (default) - take the end that sits on the NAND's input pin (-2,1)
                           and push it 20 cells west, so the compiler sees an
                           acyclic board.
         close           - take the end that is NOT on the NAND's output pin
                           (1,0) and put it exactly on (-2,1), i.e. re-form the
                           loop *in the model* after the board was compiled from
                           the cut version.  This is the C2 question: does the
                           later model change re-trigger the cycle detection?

       The board used with `close` is build/c2_cut_board.data (one NAND whose
       output leads into a dangling wire at (1,2)). */
    const char* mode=std::getenv("TC_WIRE_PROBE");
    const bool close=(mode&&std::strcmp(mode,"close")==0);
    const auto* board=static_cast<const unsigned char*>(boardModel);
    const uint64_t wires=readU64(board,0x98);
    const unsigned char* payload=readPtr(board,0xa0);
    if(!wires||!payload){
        if(host&&host->log) host->log(host->context,say("no wires on this board; nothing to cut").c_str());
        return;
    }
    auto* wire=const_cast<unsigned char*>(payload)+8;   /* element 0 */
    const int16_t x1=readI16(wire,0x18),y1=readI16(wire,0x1a);
    const int16_t x2=readI16(wire,0x1c),y2=readI16(wire,0x1e);
    if(close){
        /* Put the dangling end on the input pin (-2,1). */
        const bool firstIsOutput=(x1==1&&y1==0);
        int16_t nx1=x1,ny1=y1,nx2=x2,ny2=y2;
        if(firstIsOutput){ nx2=-2; ny2=1; } else { nx1=-2; ny1=1; }
        writeI16(wire,0x18,nx1); writeI16(wire,0x1a,ny1);
        writeI16(wire,0x1c,nx2); writeI16(wire,0x1e,ny2);
        char closed[256];
        std::snprintf(closed,sizeof(closed),
            "wires=%llu wire0=(%d,%d)->(%d,%d) now (%d,%d)->(%d,%d) [loop re-formed in the model]",
            static_cast<unsigned long long>(wires),
            static_cast<int>(x1),static_cast<int>(y1),static_cast<int>(x2),static_cast<int>(y2),
            static_cast<int>(readI16(wire,0x18)),static_cast<int>(readI16(wire,0x1a)),
            static_cast<int>(readI16(wire,0x1c)),static_cast<int>(readI16(wire,0x1e)));
        if(host&&host->log) host->log(host->context,say(closed).c_str());
        return;
    }
    /* Take the end that is closest to the NAND input pin (-2,1) and push it 20
       cells west: no segment can still touch the pin after that. */
    const bool firstIsInput=(std::abs(x1+2)+std::abs(y1-1))<=(std::abs(x2+2)+std::abs(y2-1));
    int16_t nx1=x1,ny1=y1,nx2=x2,ny2=y2;
    if(firstIsInput){ nx1=static_cast<int16_t>(x1-20); } else { nx2=static_cast<int16_t>(x2-20); }
    writeI16(wire,0x18,nx1); writeI16(wire,0x1a,ny1);
    writeI16(wire,0x1c,nx2); writeI16(wire,0x1e,ny2);
    char line[256];
    std::snprintf(line,sizeof(line),
        "wires=%llu wire0=(%d,%d)->(%d,%d) now (%d,%d)->(%d,%d) [published end pushed 20 west]",
        static_cast<unsigned long long>(wires),
        static_cast<int>(x1),static_cast<int>(y1),static_cast<int>(x2),static_cast<int>(y2),
        static_cast<int>(readI16(wire,0x18)),static_cast<int>(readI16(wire,0x1a)),
        static_cast<int>(readI16(wire,0x1c)),static_cast<int>(readI16(wire,0x1e)));
    if(host&&host->log) host->log(host->context,say(line).c_str());
}

int onLevelLoad(TCHookCall* call){
    auto* args=tc::hook::levelLoadArgs(call);
    if(args&&args->board_model){
        boardModel=args->board_model;
        framesSinceLoad=0;
        if(host&&host->log) host->log(host->context,say("level load seen, board model captured").c_str());
        /* Cut as early as possible: the game derives its connection graph from
           the model right after the load, so a patch that lands a few frames
           later is too late. */
        cutNow();
    }
    return 0;
}

void frame(void*,const TCFrame*){
    if(!boardModel) return;
    if(framesSinceLoad>=0) ++framesSinceLoad;
    if(framesSinceLoad<cutFrames) return;
    /* Fallback for the case where the load hook was not called (board up but no
       event): keep trying until the cut budget is used up. */
    static int attempts=0;
    if(attempts++>=8) return;
    cutNow();
}
} /* namespace */

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out){
    if(!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    const int status=tc::hook::addLevelLoad(h,0,&onLevelLoad,nullptr);
    if(status!=TC_HOOK_OK){
        if(h->log) h->log(h->context,say("level.load hook rejected").c_str());
        return 2;
    }
    if(h->log) h->log(h->context,say("armed; will move one wire end after the level loads").c_str());
    out->on_frame=&frame;
    return 0;
}
