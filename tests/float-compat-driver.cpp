/* Test-only entry point.  It loads the real Float Ops M0 registration first,
   then reuses the byte-adder example's isolated level runner to compile and run
   a fixture.  This file is never included in local.float-ops.mod.

   On top of that runner it drives the three phases the M0 checklist still
   needs, in order: pause the simulation so only the UI refresh path runs,
   reset, then run cycles again.  Each step is announced in the loader log so
   the probe lines can be ordered against it.

   The byte-adder self-check keeps issuing run requests of its own while it
   ramps towards its verdict, so the driver takes the simulation over as soon as
   the level runner has compiled the board: it stops the self-check and owns the
   run/pause/reset order from there. */

#include "../sdk/tc_mod_api.h"

#include <string>
#include <windows.h>

#include "../examples/float-ops/components.hpp"
#include "../sdk/tc_component_instances.h"
#include "../sdk/tc_component_storage.h"

extern "C" int float_ops_m0_embedded_load(const TCHost*, TCPlugin*);

#define tc_mod_load float_ops_level_runner_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load

namespace {

/* ---- M2: the configuration path in the real game -------------------------
   The editors write an instance's configuration through the storage service;
   this does exactly the same write, then reads the display's own text back
   through the Mod's cache.  If a configuration write did not reach the kernel,
   or the kernel's result did not reach the display, one of the three lines the
   playtest expects would not appear. */

tc::component_instances::Api m2Instances{};
tc::component_storage::Api m2Storage{};
TCCommandApiV1 m2Commands{};

bool findInstances(uint64_t custom_id, TCComponentInstanceHandle* out, uint32_t capacity,
                   uint32_t* count) {
    if (!m2Instances.enumerate) return false;
    TCComponentInstanceHandle handles[16] = {};
    uint32_t written = 0;
    if (tc::component_instances::enumerate(m2Instances, custom_id, handles, 16,
                                           &written) != TC_COMPONENT_INSTANCES_OK)
        return false;
    uint32_t copied = 0;
    for (uint32_t index = 0; index < written && copied < capacity; ++index)
        out[copied++] = handles[index];
    *count = copied;
    return copied != 0;
}

bool writeBlob(const TCComponentInstanceHandle& handle, const void* data, uint32_t bytes) {
    if (!m2Storage.write_config) return false;
    /* The registered schema, not a literal: it moves with the Mod's layout (3
       is bits/value plus the drawer's label). */
    return tc::component_storage::writeConfig(m2Storage, handle, floatops::kConfigSchema, data,
                                             bytes) ==
           TC_COMPONENT_STORAGE_OK;
}

bool requestCircuitSave() {
    TCGameHandle board{};
    if (!m2Commands.submit ||
        tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
        return false;
    TCCommandV1 save{};
    save.size = sizeof(save);
    save.type = TC_COMMAND_SAVE;
    save.subject = board;
    uint64_t request = 0;
    return m2Commands.submit(m2Commands.context, &save, &request) == TC_COMMAND_OK;
}

void logDisplay(const char* label) {
    TCComponentInstanceHandle handles[4] = {};
    uint32_t count = 0;
    if (!findInstances(floatops::kDisplayId, handles, 4, &count)) {
        log(std::string("float-ops M2 test: ") + label + " no display instance");
        return;
    }
    char text[96] = {};
    floatops::displayText(handles[0].instance_id, text, sizeof(text));
    log(std::string("float-ops M2 test: ") + label + " display shows \"" + text + "\"");
}

int floatPauseStage = 0;
bool floatPauseStarted = false;
double floatPauseStart = 0;
double floatPauseElapsed = 0;
double floatPauseStageTime = 0;
double floatCompiledAt = -1;
int64_t floatPausedCycle = -1;
unsigned long long floatPauseTicks = 0;
void (*floatOpsFrame)(void*, const TCFrame*) = nullptr;
void* floatOpsUser = nullptr;
/* Clicks the driver posts itself during the final hold: the drawer case needs
   the game's own component panel open on one of the Mod's parts, and a single
   click at a fixed time is a race against the board becoming ready. */

void floatPauseFrame(void*, const TCFrame* value) {
    /* Keep the real Mod's deferred refresh/save work alive while this test
       driver owns the package callback.  Previously the wrapper replaced the
       callback outright, so configuration writes could never exercise the
       Mod's next-frame TC_COMMAND_SAVE path. */
    if (floatOpsFrame) floatOpsFrame(floatOpsUser, value);
    frame(nullptr, value);  /* the byte-adder autotest still owns the level */
    if (!value) return;
    if (!floatPauseStarted) {
        floatPauseStarted = true;
        floatPauseStart = value->time_seconds;
    }
    floatPauseElapsed = value->time_seconds - floatPauseStart;
    if (!autotestModel || autotestStage < 2) return;
    if (floatCompiledAt < 0) {
        floatCompiledAt = floatPauseElapsed;
        log("float-compat: board compiled, driver will take the simulation over");
    }
    switch (floatPauseStage) {
        case 0:
            /* Wait for the compile request the level runner issued to finish:
               the native instances only bind when the board is compiled. */
            if (floatPauseElapsed < floatCompiledAt + 2.0) return;
            autotestDone = true;  /* the driver owns the simulation now */
            log("float-compat: driver took over the simulation");
            mod.simulation.run(autotestModel, 6);
            log("float-compat: run issued target=6");
            floatPauseStage = 1;
            floatPauseStageTime = floatPauseElapsed;
            return;
        case 1:
            if (floatPauseElapsed < floatPauseStageTime + 2.5) return;
            floatPausedCycle = mod.simulation.cycle();
            mod.simulation.pause(autotestModel);
            log("float-compat: paused after cycle=" + std::to_string(floatPausedCycle));
            floatPauseStage = 2;
            floatPauseStageTime = floatPauseElapsed;
            return;
        case 2:
            /* The pause window: the simulation must not advance, so the only
               path that runs is the UI refresh.  Poking the game's own pause
               command is what a player's pause button does; the cycle is
               watched so a window that silently kept running is visible. */
            if (floatPauseElapsed >= floatPauseStageTime + 2.5) {
                log("float-compat: pause window stable at cycle=" +
                    std::to_string(floatPausedCycle));
                mod.simulation.reset(autotestModel);
                log("float-compat: reset issued");
                floatPauseStage = 3;
                floatPauseStageTime = floatPauseElapsed;
                return;
            }
            if (floatPauseTicks++ % 30 == 0) mod.simulation.pause(autotestModel);
            if (mod.simulation.cycle() != floatPausedCycle) {
                floatPausedCycle = mod.simulation.cycle();
                log("float-compat: pause window moved to cycle=" +
                    std::to_string(floatPausedCycle));
            }
            return;
        case 3:
            if (floatPauseElapsed < floatPauseStageTime + 1.5) return;
            mod.simulation.run(autotestModel, 6);
            log("float-compat: resumed after reset, target=6");
            floatPauseStage = 4;
            floatPauseStageTime = floatPauseElapsed;
            return;
        case 4:
            if (floatPauseElapsed < floatPauseStageTime + 3.5) return;
            log("float-compat: post-reset cycles reached cycle=" +
                std::to_string(mod.simulation.cycle()));
            floatPauseStage = 5;
            return;
        /* ---- M2: the configuration path ---------------------------------- */
        case 5: {
            (void)tc::component_instances::table(host, &m2Instances);
            (void)tc::component_storage::table(host, &m2Storage);
            (void)tc::commandService(host, &m2Commands);
            TCComponentInstanceHandle constants[4] = {};
            uint32_t count = 0;
            if (!findInstances(floatops::kConstantId, constants, 4, &count) || count < 2 ||
                !m2Storage.write_config) {
                log("float-ops M2 test: the constant/adapter services are missing");
                floatPauseStage = 8;
                return;
            }
            /* The first constant carries a label, the way a player types one in
               the drawer: the board has to print it in the name slot. */
            floatops::ConstantConfig a{32, 0, 0, 0x40600000u, {}}; /* 3.5 */
            std::snprintf(a.label, sizeof(a.label), "left");
            const floatops::ConstantConfig b{32, 0, 0, 0x3FA00000u, {}}; /* 1.25 */
            const bool wrote_a = writeBlob(constants[0], &a, sizeof(a));
            const bool wrote_b = writeBlob(constants[1], &b, sizeof(b));
            log(std::string("float-ops M2 test: 3.5 and 1.25 written to the constants (") +
                (wrote_a && wrote_b ? "ok" : "failed") + ")");
            /* No run, no reset: a configuration write has to make a *paused*
               board show the new value once the editor's own refresh has been
               asked for.  The game's compiler never learns that a custom
               component's configuration moved, so the Mod's commit path issues
               the player's own refresh request (sim.do command 1) right after
               it writes - this is the same request, made here because this
               driver writes through the storage service rather than the
               drawer's field.  The read is one frame later, not in the same
               one: that refresh is a request to the simulation thread.  This is
               what a player reported missing ("the display only updates when I
               refresh"). */
            (void)mod.simulation.pause(autotestModel);
            floatPauseStage = 50;
            floatPauseStageTime = floatPauseElapsed;
            return;
        }
        case 50: {
            if (floatPauseElapsed < floatPauseStageTime + 1.0) return;
            logDisplay("right after the write, with no run,");
            (void)mod.simulation.run(autotestModel, mod.simulation.cycle() + 2);
            floatPauseStage = 6;
            floatPauseStageTime = floatPauseElapsed;
            return;
        }
        case 6: {
            if (floatPauseElapsed < floatPauseStageTime + 1.0) return;
            logDisplay("after the constants were set,");
            /* Halfway between 1.0 and the next value: the rounding box decides
               whether the result is 1 or 1.0000001. */
            TCComponentInstanceHandle constants[4] = {};
            TCComponentInstanceHandle adders[4] = {};
            uint32_t constant_count = 0, adder_count = 0;
            (void)findInstances(floatops::kConstantId, constants, 4, &constant_count);
            (void)findInstances(floatops::kAddId, adders, 4, &adder_count);
            if (constant_count < 2 || !adder_count) {
                log("float-ops M2 test: the second configuration step found no instances");
                floatPauseStage = 8;
                return;
            }
            const floatops::ConstantConfig one{32, 0, 0, 0x3F800000u, {}};  /* 1.0 */
            const floatops::ConstantConfig tiny{32, 0, 0, 0x33000000u, {}}; /* 2**-25 */
            const floatops::AddConfig rne{32, 0, 0, {}};                    /* RNE */
            const bool wrote = writeBlob(constants[0], &one, sizeof(one)) &&
                               writeBlob(constants[1], &tiny, sizeof(tiny)) &&
                               writeBlob(adders[0], &rne, sizeof(rne));
            (void)mod.simulation.run(autotestModel, mod.simulation.cycle() + 2);
            log(std::string("float-ops M2 test: halfway operands and RNE written (") +
                (wrote ? "ok" : "failed") + ")");
            floatPauseStage = 7;
            floatPauseStageTime = floatPauseElapsed;
            return;
        }
        case 7: {
            if (floatPauseElapsed < floatPauseStageTime + 1.0) return;
            logDisplay("with RNE,");
            TCComponentInstanceHandle adders[4] = {};
            uint32_t adder_count = 0;
            (void)findInstances(floatops::kAddId, adders, 4, &adder_count);
            const floatops::AddConfig rup{32, 4, 0, {}}; /* roundTowardPositive */
            const bool wrote = adder_count && writeBlob(adders[0], &rup, sizeof(rup));
            (void)mod.simulation.run(autotestModel, mod.simulation.cycle() + 2);
            log(std::string("float-ops M2 test: rounding box set to RUP (") +
                (wrote ? "ok" : "failed") + ")");
            /* Production edits pause/refresh before their one-frame-delayed
               save.  Match that ordering so the schematic writer never races
               the simulation thread while it reads component records. */
            (void)mod.simulation.pause(autotestModel);
            log(std::string("float-ops M2 test: circuit save requested (") +
                (requestCircuitSave() ? "ok" : "failed") + ")");
            floatPauseStage = 8;
            floatPauseStageTime = floatPauseElapsed;
            return;
        }
        case 8: {
            if (floatPauseElapsed < floatPauseStageTime + 1.0) return;
            logDisplay("with RUP,");
            /* Hold the board on screen for a few seconds before the case ends:
               the drawer case selects a component with the cursor driver and
               photographs the panel, and that needs the board (and this driver)
               to stay where they are. */
            floatPauseStage = 80;
            floatPauseStageTime = floatPauseElapsed;
            return;
        }
        case 80: {
            /* Keep the board up for a few seconds, clicking the adder so the
               game opens its own component drawer on one of the Mod's parts (the
               drawer case photographs that panel).  The click is repeated: the
               board is not always ready for input on the first try. */
            if (floatPauseElapsed < floatPauseStageTime + 7.0) return;
            log("float-ops M2 test: done");
            floatPauseStage = 9;
            return;
        }
        default:
            return;
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    TCPlugin floatPlugin{};
    floatPlugin.size = sizeof(floatPlugin);
    const int floatStatus = float_ops_m0_embedded_load(host, &floatPlugin);
    if (floatStatus != 0) return 100 + floatStatus;
    floatOpsFrame = floatPlugin.on_frame;
    floatOpsUser = floatPlugin.user;
    const int runnerStatus = float_ops_level_runner_load(host, plugin);
    if (runnerStatus != 0) return runnerStatus;
    plugin->on_frame = &floatPauseFrame;
    return 0;
}
