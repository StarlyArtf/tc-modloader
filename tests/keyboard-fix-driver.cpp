/* Real-machine driver for the built-in Keyboard rewrite (src/keyboard_fix.hpp).

   Nothing in the game lets an automated run press a key on a Keyboard
   component, so the driver does what the game's own recorder does: it pushes
   events into the component's ring buffer (the layout `simulator_queue_key_down`
   / `_up` and the level script's `add_keyboard_value` share - a 16-bit entry
   `down | key << 8` at the write index, stride 8, wrap 4096, write index stored
   at +4098) and then steps the simulation.

   The board is the fixture `build/kind-fixture-kbd/circuit.data`: one Keyboard
   whose read strobe is driven by a level input the sandbox cannot resolve, so
   the game's code generator emits the literal-zero strobe - the broken case the
   rewrite targets - with all three outputs wired to Output components.  On that
   board the Keyboard's output slots are 256/257 (out0), 258/259 (out1) and
   260/261 (out2); the driver reads a window around them so a layout shift shows
   up as a moved value instead of a silent pass.

   Lines are prefixed `keyboard-fix:` and the playtest asserts on them. */
#include "../sdk/tc_mod.h"
#include "../sdk/tc_simulation.h"
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace {

const TCHost* host;
tc::TCMod mod;
tc::TCSimulationModel sim;
tc::simulation::Api service{};
bool haveService=false;

constexpr uint8_t kKeyCode=0x41;          /* 'A' */
constexpr uint64_t kBufferIndex=4098;     /* write index, stride 8, wrap 4096 */
constexpr uint64_t kBufferSize=4096;
constexpr uint64_t kProbeFirst=248;
constexpr uint64_t kProbeLast=296;

int stage=0;
double stageMark=0;

void log(const std::string& message) {
    if(host) host->log(host->context,message.c_str());
}

/* Mirrors simulator_queue_key_down/_up: entry = down | key << 8. */
bool pushEvent(bool down,uint8_t key) {
    if(!sim.keyboardCharacter()) return false;
    auto* base=*static_cast<unsigned char* const*>(sim.keyboardCharacter());
    if(!base) return false;
    uint16_t write=0;
    std::memcpy(&write,base+kBufferIndex,sizeof(write));
    const uint16_t next=static_cast<uint16_t>((write+8)%kBufferSize);
    const uint16_t entry=static_cast<uint16_t>((down?1:0)|(static_cast<uint16_t>(key)<<8));
    std::memcpy(base+write,&entry,sizeof(entry));
    std::memcpy(base+kBufferIndex,&next,sizeof(next));
    return true;
}

std::string readOuts(const char* label) {
    std::ostringstream line;
    const int64_t cycle=tc::simulation::cycle(service);
    line<<label<<" cycle="<<cycle;
    for(uint64_t offset=kProbeFirst;offset<=kProbeLast;++offset) {
        uint64_t value=0;
        if(tc::simulation::read(service,offset,8,&value)!=0) continue;
        if(!value) continue;
        line<<" slot["<<offset<<"]=0x"<<std::hex<<value<<std::dec;
    }
    /* The three pins every assertion names, always printed, so a zero reads as
       a zero instead of a missing field. */
    uint64_t out0=0,out1=0,out2=0;
    tc::simulation::read(service,256,1,&out0);
    tc::simulation::read(service,258,1,&out1);
    tc::simulation::read(service,260,8,&out2);
    line<<" out0="<<out0<<" out1="<<out1<<" out2=0x"<<std::hex<<out2<<std::dec;
    return line.str();
}

/* `step`/`run_for` need the game to have run at least once (they add to the
   current cycle), while a freshly entered board sits at cycle -1.  `run_to`
   takes an absolute target and works from there: it travels the game's own
   sim.do entry, so the cycles happen on the game's simulation thread exactly
   like the player's Run button. */
void runToCycle(int64_t target) {
    const int64_t before=tc::simulation::cycle(service);
    const int result=tc::simulation::runTo(service,target);
    const int64_t after=tc::simulation::cycle(service);
    std::ostringstream line;
    line<<"keyboard-fix: runTo "<<target<<" rc="<<result<<" cycle "<<before<<" -> "<<after;
    log(line.str());
}

void frame(void*,const TCFrame* value) {
    const double time=value?value->time_seconds:0;
    if(stage==0) {
        /* The UI driver enters the board several seconds in, and the game only
           reports a cycle once the level and its board exist.  Ask for a run
           once a second until the cycle moves: a level load resets the cycle
           back to -1, so "cycle >= 0" also proves the board under test is the
           one the run happened on - and that nothing has cleared the keyboard
           buffer since. */
        if(time<stageMark+1.0) return;
        stageMark=time;
        int64_t cycle=tc::simulation::cycle(service);
        if(cycle<0) {
            runToCycle(4);
            if(time>30.0) {
                log("keyboard-fix: TIMEOUT waiting for the board to appear");
                stage=99;
            }
            return;
        }
        log("keyboard-fix: board running");
        const bool ok=pushEvent(true,kKeyCode);
        log(std::string("keyboard-fix: injected down key=0x41 ")+(ok?"ok":"FAILED"));
        runToCycle(cycle+8);
        stage=1;stageMark=time;
        return;
    }
    if(stage==99) return;
    if(stage==1&&time>=stageMark+1.0) {
        log("keyboard-fix: after-down "+readOuts("read"));
        stage=2;
        return;
    }
    if(stage==2&&time>=stageMark+1.5) {
        const bool ok=pushEvent(false,kKeyCode);
        log(std::string("keyboard-fix: injected up key=0x41 ")+(ok?"ok":"FAILED"));
        runToCycle(tc::simulation::cycle(service)+8);
        stage=3;
        return;
    }
    if(stage==3&&time>=stageMark+2.5) {
        log("keyboard-fix: after-up "+readOuts("read"));
        log("keyboard-fix: done");
        stage=4;
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* plugin) {
    if(!h||!plugin||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    if(!mod.load(h)||!mod.valid()) return 2;
    if(!sim.load(h)) {
        log("keyboard-fix: simulation model unavailable");
        return 3;
    }
    haveService=tc::simulation::table(h,&service)&&service.read_value!=nullptr;
    if(!haveService) {
        log("keyboard-fix: simulation service unavailable");
        return 4;
    }
    plugin->on_frame=frame;
    log(std::string("keyboard-fix: driver armed (character buffer ")+
        (sim.keyboardCharacter()?"resolved":"MISSING")+")");
    return 0;
}
