/* Float Ops M0: an independent Mod that measures the public component path
   before the IEEE 754 kernel is added.  These five stateless probes cover the
   shapes the production FP32 components need without changing the loader:

       source       0 in   -> Word[32]
       passthrough  Word   -> Word[32]
       result-flags Word   -> R[32] + Flags[5]
       flags-sink   Flags  -> 0 out
       sink         Word   -> 0 out

   The callbacks deliberately operate on integer bit patterns.  No host float,
   hidden simulation state or game/loader private interface is involved.

   Each probe also reports what it saw, per phase: the first cycles of every
   reset segment (one line per instance, so a binding token can be correlated
   with the value it carried), the UI refresh path (a paused board still has to
   show the right value) and every reset with the totals of the segment it
   ended. */

#include "../../sdk/tc_component_types.h"

#include "components.hpp"
#include "fp/fp32.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kRequiredHostVersion = TC_HOST_VERSION_CODE(0, 6, 0);
constexpr uint64_t kRequiredCapabilities =
    TC_CAP_LOG | TC_CAP_STATUS | TC_CAP_SERVICES | TC_CAP_GAME_HANDLES;
constexpr uint64_t kWordMask = UINT64_C(0xffffffff);
constexpr uint64_t kSourcePattern = UINT64_C(0xdeadbeef);
constexpr uint64_t kFlagsPattern = UINT64_C(0x1f);
/* Log budget per probe and per reset segment.  These lines exist to prove which
   instance carried which bit pattern, so the first cycle of a segment has to be
   covered for every instance of the board under test (the widest fixture the
   M0 tests build is 42 instances). */
constexpr uint64_t kCycleLineBudget = 256;
/* Refresh runs whenever the paused board is evaluated, so it is throttled by
   wall clock instead of by count: a pause window of a second still shows up in
   the log without flooding it.  The budget only stops a pathological case. */
constexpr uint64_t kRefreshLineBudget = 512;
constexpr int64_t kRefreshLineIntervalMs = 250;

constexpr uint64_t kSourceId = UINT64_C(0x4633325352433031);    /* F32SRC01 */
constexpr uint64_t kPassId = UINT64_C(0x4633325041535331);      /* F32PASS1 */
constexpr uint64_t kDualId = UINT64_C(0x4633324455414c31);      /* F32DUAL1 */
constexpr uint64_t kFlagsSinkId = UINT64_C(0x463332464c475331); /* F32FLGS1 */
constexpr uint64_t kSinkId = UINT64_C(0x46333253494e4b31);      /* F32SINK1 */

struct ProbeTelemetry {
    ProbeTelemetry(const char* labelValue, uint64_t expectedValue)
        : label(labelValue), expected(expectedValue) {}

    const char* label;
    uint64_t expected;
    const TCHost* host = nullptr;
    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> refreshes{0};
    std::atomic<uint64_t> resets{0};
    std::atomic<uint64_t> mismatches{0};
    std::atomic<uint64_t> cycleLines{0};
    std::atomic<uint64_t> refreshLines{0};
    std::atomic<uint64_t> refreshLogMs{0};
};

ProbeTelemetry g_sourceTelemetry{"source", kSourcePattern};
ProbeTelemetry g_passTelemetry{"pass", kSourcePattern};
ProbeTelemetry g_dualTelemetry{"result-flags", kSourcePattern};
ProbeTelemetry g_flagsTelemetry{"flags-sink", kFlagsPattern};
ProbeTelemetry g_sinkTelemetry{"sink", kSourcePattern};

/* M1: the IEEE 754 kernel ships inside this DLL.  The M0 probes do not call it
   yet, so without this check the linker would drop it and nothing would prove
   that it links and computes under the real loader (thread-local SoftFloat
   state in a plugin DLL is exactly the kind of thing that only breaks here).
   The fused case is chosen so a mul-then-add would give 0x34000000 instead of
   0x34000001, which is the difference a real FMA is supposed to make. */
int kernelSelfCheck(char* message, size_t size) {
    const tcfp::FP32Result sum = tcfp::fp32_add(0x3F800000u, 0x40000000u,
                                                tcfp::FPRounding::nearest_even);
    const tcfp::FP32Result product = tcfp::fp32_multiply(0x3F800001u, 0x3F800001u,
                                                         tcfp::FPRounding::nearest_even);
    const tcfp::FP32Result fused =
        tcfp::fp32_fused_multiply_add(0x3F800001u, 0x3F800001u, 0xBF800001u,
                                      tcfp::FPRounding::nearest_even);
    const tcfp::FP32Result division = tcfp::fp32_divide(0x3F800000u, 0x00000000u,
                                                        tcfp::FPRounding::nearest_even);
    const tcfp::FP32Result root = tcfp::fp32_square_root(0xBF800000u,
                                                         tcfp::FPRounding::nearest_even);
    std::snprintf(message, size,
                  "float-ops M1: kernel self-check 1+2=0x%08X/0x%02X "
                  "(1+ulp)^2=0x%08X/0x%02X fma=0x%08X/0x%02X 1/0=0x%08X/0x%02X "
                  "sqrt(-1)=0x%08X/0x%02X",
                  sum.bits, sum.flags, product.bits, product.flags, fused.bits,
                  fused.flags, division.bits, division.flags, root.bits, root.flags);
    if (sum.bits != 0x40400000u || sum.flags != 0) return 1;
    if (product.bits != 0x3F800002u || product.flags != tcfp::fp_flag_nx) return 1;
    if (fused.bits != 0x34000001u || fused.flags != 0) return 1;
    if (division.bits != 0x7F800000u || division.flags != tcfp::fp_flag_dz) return 1;
    if (root.bits != tcfp::kCanonicalNaN || root.flags != tcfp::fp_flag_nv) return 1;
    return 0;
}

void report(const TCHost* host, int level, const char* message) {
    if (host && host->log) host->log(host->context, message);
    if (host) (void)tc::reportStatus(host, level, message);
}

void note(const ProbeTelemetry* telemetry, const char* message) {
    if (telemetry && telemetry->host && telemetry->host->log)
        telemetry->host->log(telemetry->host->context, message);
}

int64_t steadyNowMs() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

/* The loader publishes a wide output into the state slot of its binding token,
   so a line that names the instance and the value it carried is what turns a
   token into evidence. */
void noteCycle(const TCLogicIOV2* io, ProbeTelemetry* telemetry, uint64_t observed) {
    const uint64_t lines = telemetry->cycleLines.fetch_add(1, std::memory_order_relaxed);
    if (lines >= kCycleLineBudget) return;
    char message[224]{};
    std::snprintf(message, sizeof(message),
                  "float-ops M0: %s cycle observed, instance=0x%llx cycle=%lld value=0x%08llx",
                  telemetry->label, static_cast<unsigned long long>(io->instance_id),
                  static_cast<long long>(io->cycle),
                  static_cast<unsigned long long>(observed & kWordMask));
    note(telemetry, message);
}

void noteRefresh(const TCLogicIOV2* io, ProbeTelemetry* telemetry, uint64_t observed) {
    const uint64_t lines = telemetry->refreshLines.fetch_add(1, std::memory_order_relaxed);
    const int64_t now = steadyNowMs();
    const int64_t previous =
        static_cast<int64_t>(telemetry->refreshLogMs.load(std::memory_order_relaxed));
    if (lines >= kRefreshLineBudget ||
        (lines && now - previous < kRefreshLineIntervalMs))
        return;
    telemetry->refreshLogMs.store(static_cast<uint64_t>(now), std::memory_order_relaxed);
    char message[224]{};
    std::snprintf(message, sizeof(message),
                  "float-ops M0: %s refresh observed, instance=0x%llx cycle=%lld value=0x%08llx",
                  telemetry->label, static_cast<unsigned long long>(io->instance_id),
                  static_cast<long long>(io->cycle),
                  static_cast<unsigned long long>(observed & kWordMask));
    note(telemetry, message);
}

void noteReset(const TCLogicIOV2* io, ProbeTelemetry* telemetry) {
    const uint64_t cycles = telemetry->cycles.exchange(0, std::memory_order_relaxed);
    const uint64_t refreshes = telemetry->refreshes.exchange(0, std::memory_order_relaxed);
    const uint64_t mismatches = telemetry->mismatches.exchange(0, std::memory_order_relaxed);
    telemetry->cycleLines.store(0, std::memory_order_relaxed);
    telemetry->refreshLines.store(0, std::memory_order_relaxed);
    telemetry->refreshLogMs.store(0, std::memory_order_relaxed);
    const uint64_t resets = telemetry->resets.fetch_add(1, std::memory_order_relaxed) + 1;
    char message[256]{};
    std::snprintf(message, sizeof(message),
                  "float-ops M0: %s reset #%llu, instance=0x%llx, previous cycle=%llu "
                  "refresh=%llu mismatched=%llu",
                  telemetry->label, static_cast<unsigned long long>(resets),
                  static_cast<unsigned long long>(io->instance_id),
                  static_cast<unsigned long long>(cycles),
                  static_cast<unsigned long long>(refreshes),
                  static_cast<unsigned long long>(mismatches));
    note(telemetry, message);
}

void observe(TCLogicIOV2* io, ProbeTelemetry* telemetry, uint64_t observed) {
    if (!io || !telemetry) return;
    switch (io->phase) {
        case TC_LOGIC_RESET:
            noteReset(io, telemetry);
            return;
        case TC_LOGIC_REFRESH:
            telemetry->refreshes.fetch_add(1, std::memory_order_relaxed);
            noteRefresh(io, telemetry, observed);
            return;
        case TC_LOGIC_CYCLE:
            break;
        default:
            return;
    }
    telemetry->cycles.fetch_add(1, std::memory_order_relaxed);
    if (observed != telemetry->expected)
        telemetry->mismatches.fetch_add(1, std::memory_order_relaxed);
    noteCycle(io, telemetry, observed);
}

uint64_t firstInput(const TCLogicIOV2* io) {
    return io && io->inputs && io->input_count ? io->inputs[0] & kWordMask : 0;
}

void sourceLogic(TCLogicIOV2* io) {
    if (!io) return;
    if (io->outputs && io->output_count) io->outputs[0] = kSourcePattern;
    observe(io, &g_sourceTelemetry, kSourcePattern);
}

void passLogic(TCLogicIOV2* io) {
    if (!io) return;
    const uint64_t value = firstInput(io);
    if (io->outputs && io->output_count) io->outputs[0] = value;
    observe(io, &g_passTelemetry, value);
}

void resultFlagsLogic(TCLogicIOV2* io) {
    if (!io) return;
    const uint64_t value = firstInput(io);
    if (io->outputs && io->output_count) io->outputs[0] = value;
    if (io->outputs && io->output_count > 1) io->outputs[1] = kFlagsPattern;
    observe(io, &g_dualTelemetry, value);
}

void flagsSinkLogic(TCLogicIOV2* io) {
    if (!io) return;
    observe(io, &g_flagsTelemetry, firstInput(io));
}

void sinkLogic(TCLogicIOV2* io) {
    if (!io) return;
    observe(io, &g_sinkTelemetry, firstInput(io));
}

const TCComponentPinV2 kWordInput{"word", "Word", 32, 0};
const TCComponentPinV2 kWordOutput{"word", "Word", 32, 0};
const TCComponentPinV2 kFlagsInput{"flags", "Flags", 5, 0};
const TCComponentPinV2 kResultOutputs[] = {
    {"result", "R", 32, 0},
    {"flags", "Flags", 5, 0},
};

struct ProbeDefinition {
    uint64_t customId;
    const char* typeId;
    const char* name;
    const char* description;
    const TCComponentPinV2* inputs;
    uint32_t inputCount;
    const TCComponentPinV2* outputs;
    uint32_t outputCount;
    TCLogicCallbackV2 callback;
    ProbeTelemetry* telemetry;
};

const ProbeDefinition kProbes[] = {
    /* The probes are part of this Mod, so they belong in its folder too: the
       game builds a 浮点 folder out of a name's first "/" segment, so the probes
       register as "浮点/M0/…" and land in an M0 subfolder instead of sitting
       loose in the player's 自定义 list (docs/research/palette-categories.md). */
    {kSourceId, "local.float-ops/m0-source", "浮点/M0/FP32 M0 Source",
     "M0 compatibility probe: emits the raw 32-bit pattern 0xDEADBEEF.",
     nullptr, 0, &kWordOutput, 1, &sourceLogic, &g_sourceTelemetry},
    {kPassId, "local.float-ops/m0-pass", "浮点/M0/FP32 M0 Pass",
     "M0 compatibility probe: preserves one raw 32-bit word exactly.",
     &kWordInput, 1, &kWordOutput, 1, &passLogic, &g_passTelemetry},
    {kDualId, "local.float-ops/m0-result-flags", "浮点/M0/FP32 M0 Result + Flags",
     "M0 compatibility probe: emits R[32] and a five-bit all-set Flags value.",
     &kWordInput, 1, kResultOutputs, 2, &resultFlagsLogic, &g_dualTelemetry},
    {kFlagsSinkId, "local.float-ops/m0-flags-sink", "浮点/M0/FP32 M0 Flags Sink",
     "M0 compatibility probe: consumes the five-bit Flags output of another probe.",
    &kFlagsInput, 1, nullptr, 0, &flagsSinkLogic, &g_flagsTelemetry},
    {kSinkId, "local.float-ops/m0-sink", "浮点/M0/FP32 M0 Sink",
     "M0 compatibility probe: consumes one raw 32-bit word and has no outputs.",
     &kWordInput, 1, nullptr, 0, &sinkLogic, &g_sinkTelemetry},
};

int registerProbes(const TCHost* host, const tc::component_types::Api& types) {
    for (const ProbeDefinition& probe : kProbes) {
        probe.telemetry->host = host;
        probe.telemetry->cycles.store(0, std::memory_order_relaxed);
        probe.telemetry->refreshes.store(0, std::memory_order_relaxed);
        probe.telemetry->resets.store(0, std::memory_order_relaxed);
        probe.telemetry->mismatches.store(0, std::memory_order_relaxed);
        probe.telemetry->cycleLines.store(0, std::memory_order_relaxed);
        probe.telemetry->refreshLines.store(0, std::memory_order_relaxed);
        probe.telemetry->refreshLogMs.store(0, std::memory_order_relaxed);

        TCComponentTypeDefinitionV2 definition{};
        definition.size = sizeof(definition);
        definition.version = TC_COMPONENT_TYPES_VERSION_2;
        definition.custom_id = probe.customId;
        definition.type_id = probe.typeId;
        definition.name = probe.name;
        definition.description = probe.description;
        definition.shape_svg = nullptr;
        definition.inputs = probe.inputs;
        definition.outputs = probe.outputs;
        definition.input_count = probe.inputCount;
        definition.output_count = probe.outputCount;
        definition.state_words = 0;
        definition.gate_cost = 0;
        definition.delay = 0;
        definition.callback = probe.callback;
        definition.user = probe.telemetry;

        const int status = tc::component_types::registerDefinition(types, &definition);
        if (status != TC_COMPONENT_TYPES_OK) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                          "float-ops M0: registration failed for %s: %s (%d)", probe.typeId,
                          tc::component_types::errorText(status), status);
            report(host, 2, message);
            return status;
        }
    }
    return TC_COMPONENT_TYPES_OK;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (!host || !plugin || host->api_version != TC_MOD_API_VERSION ||
        plugin->size < sizeof(TCPlugin))
        return 1;

    plugin->user = nullptr;
    plugin->on_frame = nullptr;
    plugin->on_unload = nullptr;

    if (tc::hostVersion(host) < kRequiredHostVersion) {
        report(host, 2, "float-ops M0: TC Mod Loader 0.6.0 or newer is required");
        return 2;
    }
    if (!tc::hostHas(host, kRequiredCapabilities)) {
        report(host, 2, "float-ops M0: required log/status/services capabilities are unavailable");
        return 3;
    }

    tc::component_types::Api types{};
    if (!tc::component_types::table(host, &types)) {
        report(host, 2, "float-ops M0: tc.component.types V1 is unavailable");
        return 4;
    }
    if (registerProbes(host, types) != TC_COMPONENT_TYPES_OK) return 5;

    {
        char message[320]{};
        if (kernelSelfCheck(message, sizeof(message)) != 0) {
            report(host, 2, "float-ops M1: the IEEE 754 kernel failed its load-time self-check");
            return 6;
        }
        report(host, 0, message);
    }

    if (!floatops::initialize(host, plugin)) {
        report(host, 2, "float-ops M2: the FP32 Constant/Add/Display types could not be registered");
        return 7;
    }

    report(host, 0,
           "float-ops M0: registered 32-bit source/pass/result+flags/flags-sink/word-sink "
           "probes; all are stateless");
    return 0;
}
