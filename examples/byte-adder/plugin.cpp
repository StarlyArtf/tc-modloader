// Example: a byte adder whose behaviour is decided by this C++ callback.
//
//   Carry in (1 bit) + A (8 bit) + B (8 bit)  ->  Sum (8 bit) + Carry out (1 bit)
//   Sum       = (carry_in + A + B) & 0xFF
//   Carry out = (carry_in + A + B) >> 8
//
// The loader generates the definition from the pin declarations and bridges its
// output drivers to the callback below, so the game keeps doing level judgement,
// wiring, pause and reset.  The definition's cached design statistics are fixed
// at 1 gate / 1 delay, which is what the component panel and board timing show.

#include "../../sdk/tc_mod.h"



#include <windows.h>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>

namespace {

struct V2 {
    float x, y;
};

const TCHost* host;
tc::TCMod mod;

// A few early cycles/refreshes are logged so a manual test can see the values
// moving both while the simulation runs and while only the UI is refreshing.
int logged = 0;
int peeks = 0;
int nonzeroPeeks = 0;

// Optional headless self-check: when plugin-data/example.byte-adder/autotest.txt
// contains a level name, the plugin loads that level, asks the game to compile
// and run it, and logs the verdict.  The component itself does not need this.
std::string autotestLevel;
bool autotest = false;
bool autotestStarted = false;
bool autotestDone = false;
int autotestStage = 0;
double autotestStart = 0;
double autotestStageTime = 0;
double autotestElapsed = 0;
int64_t autotestTarget = 1;
void* autotestModel = nullptr;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
using SetSimTest = void (*)(void*, int64_t, uint8_t);
using CompileRequest = void (*)(void*, const tc::TCNimString*, uint8_t);
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
using InvisibleButton = bool (*)(const char*, V2, int);
LoadLevel load_level;
SetSimTest set_sim_test;
CompileRequest compile_request;
UpdateWire update_original;
InvisibleButton invisible_original;
int autotestButtonFrame = -1;
int autotestButton = 0;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

bool hookedUpdate(void* model, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    if (!autotestModel) log("byte-adder: autotest captured the board model");
    autotestModel = model;
    return update_original ? update_original(model, context, input, point, fifth)
                           : false;
}

// The headless self-check needs the board UI to run once so the hook above can
// capture the model; pressing one of the level's invisible buttons does that,
// exactly like the custom-or example.
bool hookedInvisible(const char* id, V2 size, int flags) {
    const bool result = invisible_original(id, size, flags);
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (rva >= 0x449df0 && rva < 0x44b610) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            host->engine_proc(host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != autotestButtonFrame) {
            autotestButtonFrame = frame;
            autotestButton = 0;
        }
        ++autotestButton;
        if (autotestElapsed > 4.0 && autotestButton == 2) return true;
    }
    return result;
}

void autotestFrame(const TCFrame* frame) {
    if (!autotest || autotestDone) return;
    if (!autotestStarted) log("byte-adder: autotest first frame");
    if (!autotestStarted) {
        autotestStarted = true;
        autotestStart = frame->time_seconds;
    }
    autotestElapsed = frame->time_seconds - autotestStart;
    if (!autotestModel || autotestElapsed < 5.0) return;

    switch (autotestStage) {
        case 0: {
            tc::TCNimString name{};
            const size_t length = autotestLevel.size();
            mod.game.raw_new_string(&name, static_cast<int64_t>(length));
            name.length = length;
            std::memcpy(static_cast<unsigned char*>(name.data) + 8,
                        autotestLevel.data(), length);
            static_cast<unsigned char*>(name.data)[8 + length] = 0;
            load_level(autotestModel, &name);
            log("byte-adder: autotest loaded level " + autotestLevel);
            autotestStage = 1;
            autotestStageTime = autotestElapsed;
            return;
        }
        case 1:
            if (autotestElapsed < autotestStageTime + 2.0) return;
            if (set_sim_test) set_sim_test(autotestModel, 0, 1);
            {
                tc::TCNimString progress{};
                if (compile_request) compile_request(autotestModel, &progress, 0);
            }
            autotestStage = 2;
            autotestStageTime = autotestElapsed;
            return;
        case 2:
            if (autotestElapsed < autotestStageTime + 3.0) return;
            autotestTarget = 1;
            mod.simulation.run(autotestModel, autotestTarget);
            autotestStage = 3;
            return;
        case 3: {
            if (mod.simulation.cycle() < autotestTarget) return;
            auto* testState = reinterpret_cast<int64_t (*)()>(host->resolve_symbol(
                host->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
            const int64_t verdict = testState ? testState() : 0;
            if (verdict == 0 && autotestTarget < 39) {
                ++autotestTarget;
                mod.simulation.run(autotestModel, autotestTarget);
                return;
            }
            log("byte-adder: autotest finished cycle=" +
                std::to_string(mod.simulation.cycle()) +
                " verdict=" + std::to_string(verdict));
            autotestDone = true;
            return;
        }
        default:
            return;
    }
}

void frame(void*, const TCFrame* value) {
    autotestFrame(value);
}

// The result only depends on the inputs, so it is computed the same way for a
// simulated cycle and for the UI refresh.  Only the refresh path skips the
// per-cycle bookkeeping (logged counter / state), because refresh runs against
// a copy and must not advance anything.
void computeBytes(TCLogicIO* io) {
    const uint64_t total =
        (io->inputs[0] & 1) + (io->inputs[1] & 0xff) + (io->inputs[2] & 0xff);
    io->outputs[0] = total & 0xff;
    io->outputs[1] = (total >> 8) & 1;
}

void addBytes(TCLogicIO* io) {
    switch (io->phase) {
        case TC_LOGIC_RESET:
            logged = 0;
            io->outputs[0] = 0;
            io->outputs[1] = 0;
            log("byte-adder: reset instance=0x" + std::to_string(io->instance_id));
            return;
        case TC_LOGIC_REFRESH:
            computeBytes(io);
            if (peeks < 3 || (nonzeroPeeks < 2 &&
                              (io->inputs[0] | io->inputs[1] | io->inputs[2]))) {
                if (io->inputs[0] | io->inputs[1] | io->inputs[2]) ++nonzeroPeeks;
                ++peeks;
                log("byte-adder: peek carry_in=" + std::to_string(io->inputs[0] & 1) +
                    " a=" + std::to_string(io->inputs[1] & 0xff) +
                    " b=" + std::to_string(io->inputs[2] & 0xff) +
                    " sum=" + std::to_string(io->outputs[0]) +
                    " carry_out=" + std::to_string(io->outputs[1]));
            }
            return;
        default:
            break;
    }

    computeBytes(io);

    if (logged < 8) {
        ++logged;
        log("byte-adder: cycle=" + std::to_string(io->cycle) +
            " carry_in=" + std::to_string(io->inputs[0] & 1) +
            " a=" + std::to_string(io->inputs[1] & 0xff) +
            " b=" + std::to_string(io->inputs[2] & 0xff) +
            " sum=" + std::to_string(io->outputs[0]) +
            " carry_out=" + std::to_string(io->outputs[1]));
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    tc::TCNativeComponent component;
    component.id = 0x414444385F303031ULL;
    component.name = "Byte Adder";
    component.inputs = {{"Carry in", 1}, {"A", 8}, {"B", 8}};
    component.outputs = {{"Sum", 8}, {"Carry out", 1}};
    component.gates = 1;
    component.delay = 1;
    component.callback = &addBytes;
    const int registration = component.registerWith(h);
    if (registration != 0) {
        log("byte-adder: declarative registration failed: " + std::to_string(registration));
        return 3;
    }
    {
        std::ifstream file(std::string(h->data_directory_utf8) + "/autotest.txt");
        std::getline(file, autotestLevel);
        autotest = file && !autotestLevel.empty();
    }
    if (autotest) {
        load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
            h->context, "load_level__modelZutilities_u7740"));
        set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
            h->context, "set_sim_test__modelZutilities_u6840"));
        compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
            h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
        auto* updateTarget = h->resolve_symbol(
            h->context,
            "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
        auto* invisibleTarget = h->resolve_symbol(h->context, "igInvisibleButton");
        if (!load_level || !set_sim_test || !compile_request || !updateTarget ||
            !invisibleTarget) {
            return 6;
        }
        if (h->create_hook(h->context, updateTarget,
                           reinterpret_cast<void*>(&hookedUpdate),
                           reinterpret_cast<void**>(&update_original)) != 0) {
            return 7;
        }
        if (h->create_hook(h->context, invisibleTarget,
                           reinterpret_cast<void*>(&hookedInvisible),
                           reinterpret_cast<void**>(&invisible_original)) != 0) {
            return 8;
        }
        plugin->on_frame = frame;
    }

    log("byte-adder: registered id=" + std::to_string(component.id) +
        " declared=(" + std::to_string(component.gates) + " gate, " +
        std::to_string(component.delay) + " delay)");
    return 0;
}
