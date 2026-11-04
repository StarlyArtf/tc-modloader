// Stock simulation timing without instance edits or repeated run commands.
#define tc_mod_load cpu_benchmark_base_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load
#include <cstdio>
namespace {
void (*benchmarkBaseFrame)(void*,const TCFrame*)=nullptr;
bool benchmarkStarted=false;
double benchmarkLastTime=0;
int64_t benchmarkLastCycle=-1;
void benchmarkFrame(void* user,const TCFrame* frame) {
    if(!frame)return;
    // The example's model capture relies on an invisible UI button being
    // drawn. The host already tracks load_level's model as the Board handle;
    // resolve it instead so a different drawer layout cannot stall startup.
    if(!autotestModel) {
        TCGameHandle board{};board.size=sizeof(board);const void* model=nullptr;
        if(tc::currentGameHandle(host,TC_GAME_OBJECT_BOARD,&board)==TC_HANDLE_OK &&
           tc::resolveGameHandle(host,&board,&model)==TC_HANDLE_OK && model) {
            autotestModel=const_cast<void*>(model);
            log("CPUBENCH: model resolved from Board handle");
        }
    }
    if(benchmarkStarted)autotestDone=true;
    if(benchmarkBaseFrame)benchmarkBaseFrame(user,frame);
    const bool ready=autotestDone || (autotestStarted && frame->time_seconds-autotestStart>14);
    // The menu press is still needed to enter the first board. Once its model
    // is available, stop pressing UI buttons; internal timing is recalculated
    // by the example on its next frame.
    if(autotestModel)autotestElapsed=0;
    if(!autotestModel || !ready)return;
    if(!benchmarkStarted) {
        autotestDone=true;
        mod.simulation.setCommandSetting(2,1000000000000LL);
        mod.simulation.run(autotestModel,1000000000000LL);
        benchmarkStarted=true;
    }
    if(frame->time_seconds-benchmarkLastTime<1)return;
    benchmarkLastTime=frame->time_seconds;
    const int64_t cycle=mod.simulation.cycle();
    if(cycle<benchmarkLastCycle)log("CPUBENCH: FAIL cycle reset during benchmark");
    benchmarkLastCycle=cycle;
    char message[160];
    std::snprintf(message,sizeof(message),"CPUBENCH: t=%.9f cycle=%lld",frame->time_seconds,(long long)cycle);
    log(message);
}
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* plugin) {
    const int result=cpu_benchmark_base_load(h,plugin);
    if(result)return result;
    benchmarkBaseFrame=plugin->on_frame;plugin->on_frame=&benchmarkFrame;
    // Optional diagnostic affinity applies only to this cloned test process.
    const char* affinity=std::getenv("TC_TEST_CPU_AFFINITY");
    if(affinity && affinity[0]) {
        const DWORD_PTR mask=static_cast<DWORD_PTR>(std::strtoull(affinity,nullptr,0));
        if(!mask || !SetProcessAffinityMask(GetCurrentProcess(),mask)) {
            h->log(h->context,"CPUBENCH: FAIL diagnostic affinity");return 1;
        }
        h->log(h->context,"CPUBENCH: diagnostic process affinity applied");
    }
    return 0;
}
