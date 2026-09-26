/* Development-only probe: does a component with no inputs (a source) or no
   outputs (a sink) survive the game's importer and its scheduler?

   The loader's declarative path now *encodes* those scaffolds
   (src/component_definition.hpp) and the bridge accepts them
   (src/native_logic.hpp), but the game is the one who decides: it may refuse a
   definition without input pins, and it may drop a driver whose input or output
   is not connected to anything.  This probe registers one shape at a time with a
   callback that logs every invocation, so the loader log says which of the two
   happened - see docs/research/custom-component-pins.md.

   The level runner is the byte-adder example's (load a level, compile, run and
   judge); only the registration is replaced. */
#define tc_mod_load byte_adder_example_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../sdk/tc_component_types.h"
#include "../sdk/tc_component_instances.h"
#include "../sdk/tc_component_storage.h"

/* ---------------------------------------------------------------------------
   Wide definitions: how many pins will the game's importer actually take?

   The bridge's own limit is eight pins per direction (its payload is two 64-bit
   words, so the *total* input width is what really caps packing), but that says
   nothing about the game.  A definition with more pins is built here byte by
   byte - the same v14 layout src/component_definition.hpp writes, minus its
   eight-pin guard - and imported with the mod's own component model, so the
   answer is the importer's, not ours.  See docs/research/custom-component-pins.md.
   ------------------------------------------------------------------------- */
namespace wide {
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
    }
    void string(const char* text) {
        const size_t length = std::strlen(text);
        number(length, 2);
        bytes.insert(bytes.end(), text, text + length);
    }
    void component(unsigned kind, int x, int y, uint64_t id, const char* name, unsigned bits,
                   int ordinal) {
        number(kind, 2); number(x, 2); number(y, 2); number(0, 1); number(id, 8);
        string(name); number(ordinal ? 1 : 0, 2);
        if (ordinal) number(kind == 0x4f ? 2 : 0, 8);
        number(0, 8); number(-2 * ordinal, 2); number(bits, 8);
        number(0, 1); number(UINT64_MAX, 8); number(0, 8); number(0, 1);
        number(0, 1); number(0, 2); number(0, 2);
    }
};
inline std::vector<uint8_t> literals(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out{14};   /* component definition save format */
    uint64_t remaining = raw.size();
    do {
        uint8_t byte = uint8_t(remaining & 0x7f);
        remaining >>= 7;
        out.push_back(uint8_t(byte | (remaining ? 0x80 : 0)));
    } while (remaining);
    for (size_t at = 0; at < raw.size();) {
        const size_t chunk = raw.size() - at < 60 ? raw.size() - at : 60;
        out.push_back(uint8_t((chunk - 1) << 2));
        out.insert(out.end(), raw.begin() + at, raw.begin() + at + chunk);
        at += chunk;
    }
    return out;
}
/* n one-bit inputs, one one-bit output.  Same shape as the loader's scaffold:
   n collectors, n-1 dependency gates, one driver, and the output pin. */
inline std::vector<uint8_t> definition(unsigned n, uint64_t id) {
    Writer writer;
    writer.number(id, 8); writer.number(0, 4); writer.number(1, 8); writer.number(1, 8);
    writer.number(1, 1); writer.number(10000, 8); writer.number(0, 2); writer.string("");
    writer.number(0, 1); writer.number(0, 2); writer.number(0, 2); writer.string("");
    for (int i = 0; i < 512; ++i) writer.number(0, 1);
    /* n input pins, n collectors, n-1 dependency gates, one driver, one pin. */
    writer.number(3 * n + 1, 8);
    auto inputY = [n](unsigned i) { return int(i) * 8 - int(n - 1) * 4; };
    for (unsigned i = 0; i < n; ++i)
        writer.component(0x4f, -18, inputY(i), 0x1000 + i, "", 1, int(i) + 1);
    for (unsigned i = 0; i < n; ++i)
        writer.component(0x12, -6, inputY(i), 0x2000 + i, "", 1, 0);
    for (unsigned i = 1; i < n; ++i)
        writer.component(0x17, 40 + int(i) * 12, 100 + int(i) * 8, 0x3000 + i, "", 64, 0);
    writer.component(0x12, 4, 0, 0x4000, "", 1, 0);
    writer.component(0x51, 13, 0, 0x5000, "", 1, 1);
    /* The same wire list the loader's scaffold writes, so a refusal can only be
       about the pin count and not about a wire this probe got wrong. */
    Writer wires;
    unsigned wireCount = 0;
    auto point = [&](int x, int y) { wires.number(x, 2); wires.number(y, 2); };
    auto step = [&](int x, int y) {
        if (x) wires.number(unsigned((x > 0 ? 0 : 0x8000) | std::abs(x)), 2);
        if (y) wires.number(unsigned((y > 0 ? 0x4000 : 0xc000) | std::abs(y)), 2);
    };
    for (unsigned i = 0; i < n; ++i) {                    /* input pin -> collector */
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(-15, inputY(i)); step(8, 0); wires.number(0, 2);
    }
    int previousX = -4, previousY = inputY(0);
    for (unsigned i = 1; i < n; ++i) {                    /* dependency chain */
        const int x = 40 + int(i) * 12, y = 100 + int(i) * 8;
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(previousX, previousY); step(x - 3 - previousX, 0); step(0, y - 1 - previousY);
        step(2, 0); wires.number(0, 2);
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(-4, inputY(i)); step(x - 5 + 4, 0); step(0, y + 1 - inputY(i)); step(2, 0);
        wires.number(0, 2);
        previousX = x + 2; previousY = y;
    }
    ++wireCount;                                          /* chain -> driver input */
    wires.number(0, 1); wires.string("");
    point(previousX, previousY);
    step(4, 0);
    step(0, -80 - previousY);
    step(3 - (previousX + 4), 0);
    step(0, 80);
    wires.number(0, 2);
    ++wireCount;                                          /* driver -> output pin */
    wires.number(0, 1); wires.string("");
    point(6, 0); step(4, 0); wires.number(0, 2);
    writer.number(wireCount, 8);
    writer.bytes.insert(writer.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    return literals(writer.bytes);
}
}  // namespace wide

static TCHost probeHost;
static int (*realRegister)(void*, const TCNativeComponentDefinition*);

constexpr uint64_t kSourceId = 0x535243305F303031ULL;  /* "SRC0_001" */
constexpr uint64_t kSinkId = 0x534E4B305F303031ULL;    /* "SNK0_001" */
constexpr uint64_t kWide9Id = 0x574944395F303031ULL;   /* "WID9_001" */

static std::atomic<int> sourceCalls{0};
static std::atomic<int> sinkCalls{0};
static std::atomic<int> wide9Calls{0};
static std::atomic<int64_t> sourceLastCycle{-2};
static std::atomic<int64_t> sinkLastCycle{-2};
static std::atomic<int64_t> wide9LastCycle{-2};
static void (*wrappedFrame)(void*, const TCFrame*) = nullptr;
static bool tookOver = false;
static int64_t probeTarget = 0;

/* Once a second: where the level runner actually is.  "The callback ran once
   and then nothing" reads very differently from "the simulation never left
   cycle 0", and only the runner's own numbers can tell them apart. */
/* The instance walk (definitions further down, after registerWide9): watchFrame
   drives it, so the state and the helpers are declared here. */
using AddComponent = bool (*)(void*, void*);
using DeleteComponent = void (*)(void*, int64_t);
static AddComponent addComponent;
static DeleteComponent deleteComponent;
static int instancesStage;
static double instancesStageAt;
static TCComponentInstanceHandle firstHandle;
static TCComponentInstanceHandle secondHandle;
static tc::component_instances::Api instances;
static bool instancesReady;
static tc::component_storage::Api componentStorage;
static bool componentStorageReady;
static bool instancesReported;
static std::vector<unsigned char> wide9Placement(int16_t x, int16_t y);
static std::string describeInstances(tc::component_instances::Api& api,
                                     tc::component_instances::Handle* kept, int keep);
static int wide9BoardIndex(int ordinal);

static void watchFrame(void* user, const TCFrame* frame) {
    static double last = 0;
    if (wrappedFrame) wrappedFrame(user, frame);
    /* The level's own test stops the simulation as soon as it fails, and a
       0-pin component cannot be made to satisfy every campaign test.  Once the
       runner has reported, the probe drives a few more cycles itself so "the
       callback runs once per cycle" is measured rather than inferred. */
    /* A failed level test stops the level's own runner before it reports, so
       the probe takes over on a deadline as well: a 0-pin component cannot
       satisfy every campaign test, and "the callback runs every cycle" has to
       be measurable anyway. */
    if (autotestDone || autotestElapsed > 14.0) {
        if (!tookOver) {
            tookOver = true;
            probeTarget = mod.simulation.cycle() + 12;
            log("pin-shape: probe takes over the run up to cycle=" + std::to_string(probeTarget));
        }
        if (mod.simulation.cycle() < probeTarget)
            mod.simulation.run(autotestModel, probeTarget);
    }
    /* The instance walk: two instances are on the board, one is reset (only it
       may change), one is deleted, and the handle it left behind must be
       refused.  That is the whole point of the handle/generation contract. */
    if (tookOver && !instancesReady) {
        /* The table is resolved at load; a missing one is reported once. */
        if (!instancesReported) {
            instancesReported = true;
            log("pin-shape: tc.component.instances unavailable");
        }
    } else if (tookOver && wide9Calls.load() >= 2) {
        /* The level runner stops updating its own elapsed clock once it is
           done, so the walk keeps its own. */
        const double now = frame ? frame->time_seconds : 0.0;
        const auto requestCompile = [] {
            if (!compile_request || !autotestModel) return;
            tc::TCNimString progress{};
            compile_request(autotestModel, &progress, 0);
        };
        if (instancesStage == 0) {
            /* Both instances come from the board fixture: the two-instance case
               is the type's, not the placement helper's (measured: the helper
               refuses to place while the level's own run is in flight, and the
               count stays put). */
            log("pin-shape: instances enumerated: " + describeInstances(instances, nullptr, -1));
            TCComponentInstanceHandle handles[8]{};
            uint32_t written = 0, total = 0;
            tc::component_instances::enumerate(instances, kWide9Id, handles, 8, &written, &total);
            if (written >= 2) {
                firstHandle = handles[0];
                secondHandle = handles[1];
                uint64_t before[4]{}, after[4]{}, other[4]{};
                uint32_t beforeWords = 0, afterWords = 0, otherWords = 0;
                tc::component_instances::state(instances, firstHandle, before, 4, &beforeWords);
                uint8_t configBefore[4]{}, configAfter[4]{};
                uint32_t configBeforeBytes = 0, configAfterBytes = 0;
                TCComponentStorageInfoV1 storageInfo{};
                storageInfo.size = sizeof(storageInfo);
                const int storageInfoStatus = componentStorageReady
                    ? tc::component_storage::info(componentStorage, firstHandle, &storageInfo)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const int configReadBefore = componentStorageReady
                    ? tc::component_storage::readConfig(componentStorage, firstHandle, configBefore,
                                                        sizeof(configBefore), &configBeforeBytes)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const uint8_t replacement[4]{42, 2, 3, 4};
                const int configWrite = componentStorageReady
                    ? tc::component_storage::writeConfig(componentStorage, firstHandle, 3,
                                                         replacement, sizeof(replacement))
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const int reset = tc::component_instances::reset(instances, firstHandle);
                const int configReadAfter = componentStorageReady
                    ? tc::component_storage::readConfig(componentStorage, firstHandle, configAfter,
                                                        sizeof(configAfter), &configAfterBytes)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                storageInfo.size = sizeof(storageInfo);
                const int storageInfoAfter = componentStorageReady
                    ? tc::component_storage::info(componentStorage, firstHandle, &storageInfo)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                tc::component_instances::state(instances, firstHandle, after, 4, &afterWords);
                tc::component_instances::state(instances, secondHandle, other, 4, &otherWords);
                log("pin-shape: instances reset status=" + std::to_string(reset) +
                    " first_before=" + std::to_string(beforeWords ? before[0] : 0) +
                    " first_after=" + std::to_string(afterWords ? after[0] : 0) +
                    " second_state=" + std::to_string(otherWords ? other[0] : 0));
                log("pin-shape: storage info=" + std::to_string(storageInfoStatus) +
                    " read_before=" + std::to_string(configReadBefore) +
                    " write=" + std::to_string(configWrite) +
                    " read_after=" + std::to_string(configReadAfter) +
                    " info_after=" + std::to_string(storageInfoAfter) +
                    " schema=" + std::to_string(storageInfo.config_schema) +
                    " bytes=" + std::to_string(configAfterBytes) +
                    " before=" + std::to_string(configBeforeBytes ? configBefore[0] : 0) +
                    " after=" + std::to_string(configAfterBytes ? configAfter[0] : 0) +
                    " revision=" + std::to_string(storageInfo.config_revision));
            } else {
                log("pin-shape: instances second instance never bound (" +
                    std::to_string(written) + " of " + std::to_string(total) + ")");
            }
            instancesStage = 1;
            instancesStageAt = now;
        } else if (instancesStage == 1 && now > instancesStageAt + 1.5) {
            const int index = wide9BoardIndex(0);
            const bool removed = deleteComponent && autotestModel && index >= 0
                                     ? (deleteComponent(static_cast<char*>(autotestModel) + 0x78, index), true)
                                     : false;
            log("pin-shape: instances delete index=" + std::to_string(index) +
                (removed ? "" : " (failed)"));
            if (removed) requestCompile();
            instancesStage = 2;
            instancesStageAt = now;
        } else if (instancesStage == 2 && now > instancesStageAt + 2.0) {
            log("pin-shape: instances after delete: " + describeInstances(instances, nullptr, -1));
            auto forged = firstHandle;
            forged.generation += 1;
            log("pin-shape: instances old_handle=" +
                std::to_string(tc::component_instances::validate(instances, firstHandle)) +
                " forged_handle=" +
                std::to_string(tc::component_instances::validate(instances, forged)) +
                " (both " + std::to_string(TC_COMPONENT_INSTANCES_ERR_STALE) + " = stale)");
            log("pin-shape: instances stage done");
            instancesStage = 3;
        }
    }
    if (!frame || frame->time_seconds - last < 1.0) return;
    last = frame->time_seconds;
    using SimGetTestState = int64_t (*)();
    auto* testState = reinterpret_cast<SimGetTestState>(
        host->resolve_symbol(host->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
    log("pin-shape: watch t=" + std::to_string(static_cast<int>(frame->time_seconds)) +
        " cycle=" + std::to_string(mod.simulation.cycle()) +
        " stage=" + std::to_string(autotestStage) +
        " target=" + std::to_string(autotestTarget) +
        " verdict=" + std::to_string(testState ? testState() : -1));
}

static void sourceCallback(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: source reset instance=" + std::to_string(io->instance_id));
        io->outputs[0] = 0;
        return;
    }
    /* A steady zero.  The board drives the level's output with OR(input, this),
       so the level's own test passes and keeps the run going; what the probe
       reports is whether the callback is reached *every cycle*, which the call
       count next to the cycle number shows. */
    io->outputs[0] = 0;
    /* One line per cycle that reaches the callback.  Refresh peeks run the
       callback too (the panel needs the value), so only the cycle phase counts
       here - otherwise "one call per cycle" would be a claim about frames. */
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = sourceCalls.fetch_add(1) + 1;
    if (io->cycle != sourceLastCycle.exchange(io->cycle))
        log("pin-shape: source cycle=" + std::to_string(io->cycle) +
            " instance=" + std::to_string(io->instance_id) +
            " calls=" + std::to_string(calls));
}

static void sinkCallback(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: sink reset instance=" + std::to_string(io->instance_id));
        return;
    }
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = sinkCalls.fetch_add(1) + 1;
    if (io->cycle != sinkLastCycle.exchange(io->cycle))
        log("pin-shape: sink cycle=" + std::to_string(io->cycle) +
            " instance=" + std::to_string(io->instance_id) +
            " in=" + std::to_string(io->inputs[0]) + " calls=" + std::to_string(calls));
}

/* The V2 shape: nine inputs and one output through tc.component.types, with the
   definition - not a fixed eight words - choosing the state size. */
static void wide9Callback(TCLogicIOV2* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: wide9 reset instance=" + std::to_string(io->instance_id) +
            " state_words=" + std::to_string(io->state_words));
        if (io->state && io->state_words) io->state[0] = 0;
        if (io->outputs) io->outputs[0] = 0;
        return;
    }
    if (io->outputs) io->outputs[0] = 0;   /* or_gate passes while this stays 0 */
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = wide9Calls.fetch_add(1) + 1;
    if (io->cycle != wide9LastCycle.exchange(io->cycle)) {
        std::string inputs;
        for (uint32_t i = 0; i < io->input_count; ++i) {
            if (i) inputs += ",";
            inputs += std::to_string(io->inputs ? io->inputs[i] : 0);
        }
        log("pin-shape: wide9 cycle=" + std::to_string(io->cycle) +
            " inputs=" + std::to_string(io->input_count) + "[" + inputs + "]" +
            " state=" + std::to_string(io->state && io->state_words ? io->state[0] : 0) +
            " calls=" + std::to_string(calls));
    }
    if (io->state && io->state_words) io->state[0] = io->state[0] + 1;
}

/* Registers the nine-input shape through the V2 service (not through
   register_component, whose callback struct carries eight pins). */
/* Lifecycle: the host announces an instance when it first binds it and before
   it is released.  Both run where the change happened, and both are allowed to
   write state. */
static void wide9Create(TCLogicIOV2* io) {
    if (io->state && io->state_words) io->state[0] = 0x1000;
    log("pin-shape: wide9 on_create instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase) + " state_words=" + std::to_string(io->state_words));
}
static void wide9Destroy(TCLogicIOV2* io) {
    log("pin-shape: wide9 on_destroy instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase) +
        " state=" + std::to_string(io->state && io->state_words ? io->state[0] : 0));
}
static const TCComponentLifecycleV1 wide9Lifecycle = {
    sizeof(TCComponentLifecycleV1), TC_COMPONENT_LIFECYCLE_VERSION_1,
    &wide9Create, &wide9Destroy
};
static const uint8_t wide9DefaultConfig[4] = {7, 2, 3, 4};

static void registerWide9(const TCHost* h) {
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) {
        log("pin-shape: tc.component.types unavailable; wide9 not registered");
        return;
    }
    static const TCComponentPinV2 ins[9] = {
        {"in0","Input 0",1,0}, {"in1","Input 1",1,0}, {"in2","Input 2",1,0},
        {"in3","Input 3",1,0}, {"in4","Input 4",1,0}, {"in5","Input 5",1,0},
        {"in6","Input 6",1,0}, {"in7","Input 7",1,0}, {"in8","Input 8",1,0}};
    static const TCComponentPinV2 outs[1] = {{"out","Output",1,0}};
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kWide9Id;
    definition.type_id = "dev.pin-shape-probe/wide9";
    definition.name = "Pin shape wide9";
    definition.description = "9 inputs, 1 output, 2 state words";
    definition.inputs = ins; definition.input_count = 9;
    definition.outputs = outs; definition.output_count = 1;
    definition.state_words = 2;
    definition.gate_cost = 1; definition.delay = 1;
    definition.callback = &wide9Callback;
    definition.lifecycle = &wide9Lifecycle;
    definition.config_schema = 3;
    definition.config_size = sizeof(wide9DefaultConfig);
    definition.default_config = wide9DefaultConfig;
    const int result = tc::component_types::registerDefinition(types, &definition);
    log("pin-shape: wide9 V2 registration (9in/1out) -> " + std::to_string(result) +
        " (" + tc::component_types::errorText(result) + ")");
}

/* ---- instances: placement, queries, reset and destroy ---------------------

   The nine-pin board starts with one instance.  This puts a second one on the
   board at run time (the same placement template the component menu uses), then
   drives the tc.component.instances service against both, resets one, deletes
   the other and checks the handle it left behind is refused. */
static std::vector<unsigned char> wide9Placement(int16_t x, int16_t y) {
    std::vector<unsigned char> placement(0x238, 0);
    placement[0] = 0x4E;                                  /* custom instance */
    const int32_t point = static_cast<int32_t>((static_cast<uint32_t>(y) << 16) |
                                               static_cast<uint16_t>(x));
    std::memcpy(placement.data() + 2, &point, sizeof(point));
    const uint64_t id = kWide9Id;
    std::memcpy(placement.data() + 0x188, &id, sizeof(id));
    const uint64_t one = 1, capacity = 0x100;
    std::memcpy(placement.data() + 0x58, &one, sizeof(one));
    std::memcpy(placement.data() + 0x60, &capacity, sizeof(capacity));
    placement[0x68] = 1;
    std::memcpy(placement.data() + 0x70, &one, sizeof(one));
    std::memcpy(placement.data() + 0x78, &capacity, sizeof(capacity));
    placement[0x80] = 1;
    return placement;
}

/* Index of one of our components in the board's own component array. */
static int wide9BoardIndex(int ordinal) {
    if (!autotestModel) return -1;
    const auto* base = static_cast<const unsigned char*>(autotestModel);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, base + 0x78, sizeof(count));
    std::memcpy(&data, base + 0x80, sizeof(data));
    if (!data || count > 100000) return -1;
    int seen = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const unsigned char* record = data + 8 + i * 0x238;
        if (record[0] != 0x4e) continue;
        uint64_t id = 0;
        std::memcpy(&id, record + 0x188, sizeof(id));
        if (id != kWide9Id) continue;
        if (seen++ == ordinal) return static_cast<int>(i);
    }
    return -1;
}

static std::string describeInstances(tc::component_instances::Api& instances,
                                     tc::component_instances::Handle* kept, int keep) {
    TCComponentInstanceHandle handles[8]{};
    uint32_t written = 0, total = 0;
    const int status = tc::component_instances::enumerate(instances, kWide9Id, handles, 8,
                                                          &written, &total);
    std::string text = "enum status=" + std::to_string(status) + " written=" +
                       std::to_string(written) + " total=" + std::to_string(total);
    for (uint32_t i = 0; i < written && i < 2; ++i) {
        TCComponentInstanceInfoV1 info{};
        info.size = sizeof(info);
        const int infoStatus = tc::component_instances::info(instances, handles[i], &info);
        uint64_t state[4]{};
        uint32_t words = 0;
        const int stateStatus = tc::component_instances::state(instances, handles[i], state, 4,
                                                               &words);
        text += " | #" + std::to_string(i) + " instance=0x" + [&] {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%llx",
                          static_cast<unsigned long long>(handles[i].instance_id));
            return std::string(buffer);
        }() + " gen=" + std::to_string(handles[i].generation) +
            " info=" + std::to_string(infoStatus) + " calls=" + std::to_string(info.cycle_calls) +
            " state_status=" + std::to_string(stateStatus) + " words=" + std::to_string(words) +
            " state0=" + std::to_string(words ? state[0] : 0);
        if (kept && static_cast<int>(i) == keep) *kept = handles[i];
    }
    return text;
}

/* Registers the two shapes instead of the example's own component.  A refusal
   is reported and left in place: which shape the game accepts *is* the
   measurement, so the probe does not paper over it. */
static int registerShapes(void* context, const TCNativeComponentDefinition* source) {
    static const TCComponentPin outPin{"Out", 1};
    static const TCComponentPin inPin{"In", 1};
    TCNativeComponentDefinition definition = *source;
    definition.custom_id = kSourceId;
    definition.name = "Pin shape source";
    definition.description = "0 inputs, 1 output";
    definition.inputs = nullptr;
    definition.input_count = 0;
    definition.outputs = &outPin;
    definition.output_count = 1;
    definition.callback = &sourceCallback;
    definition.shape_svg = nullptr;
    const int sourceResult = realRegister(context, &definition);
    log("pin-shape: source registration (0in/1out) -> " + std::to_string(sourceResult));

    definition.custom_id = kSinkId;
    definition.name = "Pin shape sink";
    definition.description = "1 input, 0 outputs";
    definition.inputs = &inPin;
    definition.input_count = 1;
    definition.outputs = nullptr;
    definition.output_count = 0;
    definition.callback = &sinkCallback;
    const int sinkResult = realRegister(context, &definition);
    log("pin-shape: sink registration (1in/0out) -> " + std::to_string(sinkResult));

    /* The level runner only needs one component to exist; a refusal of both is
       still reported through the log above, which is what the probe is for. */
    return sourceResult == 0 ? 0 : (sinkResult == 0 ? 0 : sourceResult);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin) return 1;
    probeHost = *h;
    realRegister = h->register_component;
    probeHost.register_component = &registerShapes;
    const int result = byte_adder_example_load(&probeHost, plugin);
    if (result == 0 && plugin->on_frame) {
        wrappedFrame = plugin->on_frame;
        plugin->on_frame = &watchFrame;
    }
    if (result == 0) registerWide9(h);
    if (result == 0) {
        instancesReady = tc::component_instances::table(h, &instances);
        if (!instancesReady) log("pin-shape: tc.component.instances unavailable");
        componentStorageReady = tc::component_storage::table(h, &componentStorage);
        if (!componentStorageReady) log("pin-shape: tc.component.storage unavailable");
        addComponent = reinterpret_cast<AddComponent>(h->resolve_symbol(
            h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
        deleteComponent = reinterpret_cast<DeleteComponent>(h->resolve_symbol(
            h->context, "board_delete_component__modelZboardZboard_u10711"));
    }
    /* Last, so a refused import cannot block the shapes above: the importer
       leaves the game's error flag set when it rejects a definition. */
    if (result == 0) {
        /* The import entry wants a directory that ends in a separator; the host
           hands out the mod's own data directory without one. */
        const std::string directory = std::string(h->data_directory_utf8) + "/";
        uint64_t id = 0x5749444500000000ULL;
        for (unsigned pins : {9u, 16u, 32u}) {
            const auto bytes = wide::definition(pins, id);
            const auto imported = mod.components.importCircuit(
                "Pin shape wide", bytes.data(), bytes.size(), directory.c_str());
            log("pin-shape: wide import " + std::to_string(pins) + "in/1out -> status " +
                std::to_string(static_cast<int>(imported.status)) + " id=" +
                std::to_string(imported.custom_id));
            if (!imported.ok()) break;
            ++id;
        }
    }
    return result;
}
