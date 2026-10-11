// 仿真测速探针：把仿真持续跑起来，并每秒把 cycles/sec 写进 loader.log。
//
//   装好、重启游戏后，切到不同关卡各停几秒，日志里就会出现
//       speed-probe: 12345 cycles/sec  (cycle=678901)
//   配合日志里前面的 "Level loaded: <关卡>" 就能对上号。它不注册元件、
//   不改存档；只用 tc.simulation 的 run_for/cycle（关卡默认是暂停的，
//   不主动跑起来就没有速率可测）。
#include "../../sdk/tc_mod.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

const TCHost* host = nullptr;
tc::TCMod mod;
tc::simulation::Api sim{};
bool simReady = false;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

void probeFrame(void*, const TCFrame*) {
    static uint64_t lastTick = 0;
    static long long lastCycle = 0;
    const uint64_t now = GetTickCount64();
    if (!lastTick) {                       // 第一帧只记基准
        lastTick = now;
        lastCycle = mod.simulation.cycle();
        if (simReady) tc::simulation::runFor(sim, 2000000);   // 让它一直往前跑
        return;
    }
    if (now - lastTick < 1000) return;     // 每秒一行
    const long long cycle = mod.simulation.cycle();
    const double seconds = static_cast<double>(now - lastTick) / 1000.0;
    char line[192]{};
    std::snprintf(line, sizeof(line), "speed-probe: %.0f cycles/sec  (cycle=%lld)",
                  static_cast<double>(cycle - lastCycle) / seconds, cycle);
    log(line);
    if (simReady) tc::simulation::runFor(sim, 2000000);       // 每秒续一次
    lastTick = now;
    lastCycle = cycle;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    simReady = tc::simulation::table(h, &sim);
    plugin->on_frame = probeFrame;
    log(simReady ? "speed-probe: armed - 自己驱动仿真，每秒记录一次 cycles/sec"
                 : "speed-probe: armed - 本加载器没有仿真控制，只读 cycle");
    return 0;
}
