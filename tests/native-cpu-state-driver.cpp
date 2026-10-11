// Deterministic state comparison for an actual stock CPU in a cloned save.
#define tc_mod_load cpu_state_base_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load
#include "../sdk/tc_simulation.h"
#include "../sdk/tc_sim_capture.h"
#include <array>
#include <fstream>
#include <iomanip>

namespace {
void (*cpuStateFrame)(void*,const TCFrame*)=nullptr;
tc::simulation::Api sim{};
tc::sim_capture::Api capture{};
std::array<tc::simulation::Channel,64> channels{};
std::ofstream output;
int phase=0;
int64_t target=0;
double phaseAt=0;
void fail(const std::string& reason) {log("CPUSTATE: FAIL "+reason);phase=99;}
void stateFrame(void* user,const TCFrame* frame) {
    if(!autotestModel) {
        TCGameHandle board{};board.size=sizeof(board);const void* model=nullptr;
        if(tc::currentGameHandle(host,TC_GAME_OBJECT_BOARD,&board)==TC_HANDLE_OK &&
           tc::resolveGameHandle(host,&board,&model)==TC_HANDLE_OK && model)
            autotestModel=const_cast<void*>(model);
    }
    if(cpuStateFrame)cpuStateFrame(user,frame);
    if(!frame)return;
    const bool ready=autotestDone || (autotestStarted && frame->time_seconds-autotestStart>14);
    if(autotestModel)autotestElapsed=0; // menu bootstrap is no longer needed
    if(!autotestModel || !ready || phase==99)return;
    autotestDone=true;
    const auto cycle=mod.simulation.cycle();
    if(phase==0) {
        mod.simulation.setCommandSetting(2,1000000000000LL);
        mod.simulation.reset(autotestModel);phaseAt=frame->time_seconds;phase=1;return;
    }
    if(phase==1) {
        if(frame->time_seconds-phaseAt<0.2)return;
        if(cycle!=-1){fail("reset cycle="+std::to_string(cycle));return;}
        if(tc::sim_capture::configure(capture,channels.data(),unsigned(channels.size()),8192)!=TC_SIM_CAPTURE_OK ||
           tc::sim_capture::start(capture)!=TC_SIM_CAPTURE_OK){fail("capture setup");return;}
        target=4096;mod.simulation.run(autotestModel,target);phase=2;phaseAt=frame->time_seconds;return;
    }
    if(cycle<target) {
        if(frame->time_seconds-phaseAt>5)fail("target timeout");
        return;
    }
    if(cycle!=target){fail("overshoot");return;}
    tc::sim_capture::stop(capture);
    TCCaptureStatusV1 status{};
    // The initial cycle is -1. Its unsigned tick wraps to UINT64_MAX;
    // transitioning to cycle 0 starts the actual contiguous capture once.
    if(tc::sim_capture::status(capture,&status)!=TC_SIM_CAPTURE_OK || status.rows!=4096 || status.first_cycle!=0 || status.last_cycle!=4095 || status.gaps || status.restarts!=1){fail("incomplete cycle capture rows="+std::to_string(status.rows)+" first="+std::to_string(status.first_cycle)+" last="+std::to_string(status.last_cycle)+" gaps="+std::to_string(status.gaps)+" restarts="+std::to_string(status.restarts));return;}
    std::vector<uint64_t> cycles(4096),values(4096*channels.size());uint32_t rows=4096;
    if(tc::sim_capture::read(capture,cycles.data(),values.data(),4096,&rows)!=TC_SIM_CAPTURE_OK || rows!=4096){fail("capture read");return;}
    for(uint32_t row=0;row<rows;++row) {
        if(cycles[row]!=row){fail("non-consecutive cycles");return;}
        output<<std::dec<<cycles[row];
        for(size_t col=0;col<channels.size();++col)output<<','<<std::hex<<std::setw(16)<<std::setfill('0')<<values[row*channels.size()+col];
        output<<'\n';
    }
    output.close();
    if(!output){fail("state file write");return;}
    log("CPUSTATE: PASS rows="+std::to_string(rows)+" channels=64 final=4095 gaps=0");phase=99;
}
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* plugin) {
    const int result=cpu_state_base_load(h,plugin);
    if(result)return result;
    if(!tc::simulation::table(h,&sim))return 2;
    if(!tc::sim_capture::table(h,&capture))return 2;
    for(size_t i=0;i<channels.size();++i) {
        channels[i].size=sizeof(channels[i]);channels[i].version=TCSIM_CHANNEL_VERSION_1;
        channels[i].byte_offset=256+i*8;channels[i].bits=64;
    }
    output.open(std::string(h->data_directory_utf8)+"/cpu-state.csv");
    if(!output)return 3;
    cpuStateFrame=plugin->on_frame;plugin->on_frame=&stateFrame;
    return 0;
}
