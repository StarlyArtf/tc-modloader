#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define tc_mod_load float_ops_m0_test_load
#include "../examples/float-ops/plugin.cpp"
#undef tc_mod_load

namespace {

struct CapturedDefinition {
    TCComponentTypeDefinitionV2 definition{};
    std::vector<TCComponentPinV2> inputs;
    std::vector<TCComponentPinV2> outputs;
};

struct FakeHost {
    std::vector<CapturedDefinition> definitions;
    std::vector<std::string> messages;
    bool provideTypes = true;
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "float-compat: " << message << '\n';
    std::exit(1);
}

void require(bool value, const std::string& message) {
    if (!value) fail(message);
}

void fakeLog(void* context, const char* message) {
    auto* fake = static_cast<FakeHost*>(context);
    fake->messages.emplace_back(message ? message : "");
}

int fakeStatus(void* context, int, const char* message) {
    fakeLog(context, message);
    return 0;
}

int captureDefinition(void* context, const TCComponentTypeDefinitionV2* source) {
    if (!context || !source) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    auto* fake = static_cast<FakeHost*>(context);
    CapturedDefinition captured{};
    captured.definition = *source;
    if (source->inputs && source->input_count)
        captured.inputs.assign(source->inputs, source->inputs + source->input_count);
    if (source->outputs && source->output_count)
        captured.outputs.assign(source->outputs, source->outputs + source->output_count);
    captured.definition.inputs = captured.inputs.empty() ? nullptr : captured.inputs.data();
    captured.definition.outputs = captured.outputs.empty() ? nullptr : captured.outputs.data();
    fake->definitions.push_back(std::move(captured));
    return TC_COMPONENT_TYPES_OK;
}

int fakeQueryService(void* context, const char* id, uint32_t version, void* out,
                     uint32_t outSize) {
    auto* fake = static_cast<FakeHost*>(context);
    if (!fake || !fake->provideTypes || !id ||
        std::strcmp(id, TC_SERVICE_COMPONENT_TYPES) != 0)
        return TC_SERVICE_ERR_UNAVAILABLE;
    if (version != TC_COMPONENT_TYPES_API_VERSION_1) return TC_SERVICE_ERR_VERSION;
    if (!out || outSize < sizeof(TCComponentTypesApiV1)) return TC_SERVICE_ERR_SIZE;
    TCComponentTypesApiV1 table{};
    table.size = sizeof(table);
    table.version = TC_COMPONENT_TYPES_API_VERSION_1;
    table.context = fake;
    table.register_definition = &captureDefinition;
    std::memcpy(out, &table, sizeof(table));
    return TC_SERVICE_OK;
}

TCHost makeHost(FakeHost* fake) {
    TCHost host{};
    host.size = sizeof(host);
    host.api_version = TC_MOD_API_VERSION;
    host.context = fake;
    host.game_build = "offline-test";
    host.mod_id = "local.float-ops";
    host.data_directory_utf8 = "";
    host.log = &fakeLog;
    host.host_version = TC_HOST_VERSION_CODE(0, 6, 0);
    host.capabilities =
        TC_CAP_LOG | TC_CAP_STATUS | TC_CAP_SERVICES | TC_CAP_GAME_HANDLES;
    host.report_status = &fakeStatus;
    host.query_service = &fakeQueryService;
    return host;
}

const CapturedDefinition& findDefinition(const FakeHost& fake, const char* typeId) {
    for (const CapturedDefinition& captured : fake.definitions)
        if (captured.definition.type_id && std::strcmp(captured.definition.type_id, typeId) == 0)
            return captured;
    fail(std::string("missing definition ") + typeId);
}

void invoke(const CapturedDefinition& captured, uint32_t phase, const uint64_t* inputs,
            uint32_t inputCount, uint64_t* outputs, uint32_t outputCount,
            const void* config = nullptr, uint32_t configSize = 0,
            uint32_t configSchema = 0) {
    TCLogicIOV2 io{};
    io.size = sizeof(io);
    io.version = TC_LOGIC_IO_V2_VERSION_1;
    io.phase = phase;
    io.instance_id = UINT64_C(0x12345678);
    io.cycle = 7;
    io.input_count = inputCount;
    io.output_count = outputCount;
    io.inputs = inputs;
    io.outputs = outputs;
    io.state = nullptr;
    io.state_words = 0;
    io.user = captured.definition.user;
    io.config = static_cast<const uint8_t*>(config);
    io.config_size = configSize;
    io.config_schema = configSchema;
    captured.definition.callback(&io);
}

void verifyShape(const CapturedDefinition& captured, uint32_t inputs, uint32_t outputs) {
    require(captured.definition.size == sizeof(TCComponentTypeDefinitionV2),
            "definition size mismatch");
    require(captured.definition.version == TC_COMPONENT_TYPES_VERSION_2,
            "definition version mismatch");
    require(captured.definition.input_count == inputs, "input count mismatch");
    require(captured.definition.output_count == outputs, "output count mismatch");
    require(captured.definition.state_words == 0, "M0 probes must be stateless");
    require(captured.definition.callback != nullptr, "missing callback");
}

bool mentions(const FakeHost& fake, const std::string& text) {
    for (const std::string& message : fake.messages)
        if (message.find(text) != std::string::npos) return true;
    return false;
}

}  // namespace

int main() {
    {
        FakeHost unavailable{};
        unavailable.provideTypes = false;
        TCHost host = makeHost(&unavailable);
        TCPlugin plugin{};
        plugin.size = sizeof(plugin);
        require(float_ops_m0_test_load(&host, &plugin) != 0,
                "load must fail when tc.component.types is unavailable");
    }

    FakeHost fake{};
    TCHost host = makeHost(&fake);
    TCPlugin plugin{};
    plugin.size = sizeof(plugin);
    require(float_ops_m0_test_load(&host, &plugin) == 0, "plugin load failed");
    /* Five M0 probes, the three M2 components and the M3/M4 catalogue. */
    require(fake.definitions.size() == 8 + floatops::catalogueCount(),
            "expected the five M0 probes plus the three M2 components");

    const auto& source = findDefinition(fake, "local.float-ops/m0-source");
    const auto& pass = findDefinition(fake, "local.float-ops/m0-pass");
    const auto& dual = findDefinition(fake, "local.float-ops/m0-result-flags");
    const auto& flagsSink = findDefinition(fake, "local.float-ops/m0-flags-sink");
    const auto& sink = findDefinition(fake, "local.float-ops/m0-sink");
    verifyShape(source, 0, 1);
    verifyShape(pass, 1, 1);
    verifyShape(dual, 1, 2);
    verifyShape(flagsSink, 1, 0);
    verifyShape(sink, 1, 0);

    require(source.outputs.size() == 1 && source.outputs[0].bits == 32,
            "source must expose one 32-bit output");
    require(pass.inputs.size() == 1 && pass.inputs[0].bits == 32 &&
                pass.outputs.size() == 1 && pass.outputs[0].bits == 32,
            "pass must be 32 -> 32");
    require(dual.inputs.size() == 1 && dual.inputs[0].bits == 32 &&
                dual.outputs.size() == 2 && dual.outputs[0].bits == 32 &&
                dual.outputs[1].bits == 5,
            "dual probe must be Word[32] -> R[32] + Flags[5]");
    require(flagsSink.inputs.size() == 1 && flagsSink.inputs[0].bits == 5 &&
                flagsSink.outputs.empty(),
            "flags sink must be Flags[5] -> no outputs");
    require(sink.inputs.size() == 1 && sink.inputs[0].bits == 32 && sink.outputs.empty(),
            "sink must be 32 -> no outputs");

    constexpr uint64_t vectors[] = {
        UINT64_C(0x00000000), UINT64_C(0x00000001), UINT64_C(0x7f7fffff),
        UINT64_C(0x7f800000), UINT64_C(0x7fc00000), UINT64_C(0x80000000),
        UINT64_C(0xffffffff), UINT64_C(0xdeadbeef),
    };
    for (uint32_t phase : {uint32_t(TC_LOGIC_RESET), uint32_t(TC_LOGIC_REFRESH),
                           uint32_t(TC_LOGIC_CYCLE)}) {
        uint64_t sourceOut = 0;
        invoke(source, phase, nullptr, 0, &sourceOut, 1);
        require(sourceOut == UINT64_C(0xdeadbeef), "source pattern changed");
        for (uint64_t vector : vectors) {
            uint64_t passOut = 0;
            invoke(pass, phase, &vector, 1, &passOut, 1);
            require(passOut == vector, "pass probe changed a bit");

            uint64_t dualOut[2]{};
            invoke(dual, phase, &vector, 1, dualOut, 2);
            require(dualOut[0] == vector, "dual result changed a bit");
            require(dualOut[1] == UINT64_C(0x1f), "dual flags changed");

            invoke(sink, phase, &vector, 1, nullptr, 0);

            /* The flags sink only ever sees the five-bit pattern the dual probe
               publishes, so its own expectation is that pattern - not the 32-bit
               word. */
            const uint64_t flags = UINT64_C(0x1f);
            invoke(flagsSink, phase, &flags, 1, nullptr, 0);
        }
    }

    /* The probes report what each phase observed; the offline host keeps those
       lines so the true-game assertions have a fast counterpart. */
    for (const char* label : {"source", "pass", "result-flags", "flags-sink", "sink"}) {
        require(mentions(fake, std::string("float-ops M0: ") + label + " reset #1"),
                std::string("missing reset report for ") + label);
        require(mentions(fake, std::string("float-ops M0: ") + label + " refresh observed"),
                std::string("missing refresh report for ") + label);
    }
    require(mentions(fake, "float-ops M0: flags-sink cycle observed"),
            "the flags sink never reported a cycle observation");

    /* ---- M2: the vertical slice, offline ----------------------------------
       The same definitions the game registers, driven through the callback with
       the configuration the editors write. */
    const auto& constant = findDefinition(fake, "local.float-ops/fp32-constant");
    const auto& add = findDefinition(fake, "local.float-ops/fp32-add");
    const auto& display = findDefinition(fake, "local.float-ops/fp32-display");
    verifyShape(constant, 0, 1);
    verifyShape(add, 2, 2);
    verifyShape(display, 1, 0);
    require(constant.definition.config_size == sizeof(floatops::ConstantConfig) &&
                constant.definition.config_schema == 3 &&
                constant.definition.default_config != nullptr,
            "the constant must carry a schema-3 configuration (bits + label)");
    require(add.definition.config_size == sizeof(floatops::AddConfig) &&
                add.definition.config_schema == 3,
            "the adder must carry a schema-3 configuration (rounding + label)");
    require(display.definition.config_size == sizeof(floatops::DisplayConfig) &&
                display.definition.config_schema == 3,
            "the display must carry a schema-3 configuration (mode + label)");
    require(add.outputs.size() == 2 && add.outputs[0].bits == 32 &&
                add.outputs[1].bits == 5,
            "the adder must publish R[32] and Flags[5]");
    require(constant.definition.lifecycle != nullptr &&
                constant.definition.lifecycle->on_create != nullptr &&
                constant.definition.lifecycle->on_config_changed != nullptr &&
                constant.definition.lifecycle->on_clone != nullptr &&
                constant.definition.lifecycle->on_load != nullptr,
            "the constant must refresh after bind, edit, clone and level reload");

    const floatops::ConstantConfig constant_config{32, 0, 0, 0xDEADBEEFu, {}};
    uint64_t constant_output = 0;
    invoke(constant, TC_LOGIC_CYCLE, nullptr, 0, &constant_output, 1, &constant_config,
           sizeof(constant_config), 3);
    require(constant_output == 0xDEADBEEFu,
            "the constant did not publish its configured bits");

    /* A level re-entry binds the instance before the paused board necessarily
       evaluates it.  The lifecycle callback must prime the configured output
       immediately and arm the next-frame board refresh; selection/clicking is
       not part of this path. */
    constant_output = 0;
    TCLogicIOV2 rebound{};
    rebound.size = sizeof(rebound);
    rebound.version = TC_LOGIC_IO_V2_VERSION_1;
    rebound.phase = TC_LOGIC_LOAD;
    rebound.instance_id = UINT64_C(0x12345678);
    rebound.output_count = 1;
    rebound.outputs = &constant_output;
    rebound.config = reinterpret_cast<const uint8_t*>(&constant_config);
    rebound.config_size = sizeof(constant_config);
    rebound.config_schema = 3;
    constant.definition.lifecycle->on_load(&rebound);
    require(constant_output == 0xDEADBEEFu,
            "a reloaded constant kept a stale output until interaction");

    const floatops::ConstantConfig edited_config{32, 0, 0, 0x40980000u, {}};
    rebound.phase = TC_LOGIC_CONFIG_CHANGED;
    rebound.config = reinterpret_cast<const uint8_t*>(&edited_config);
    constant.definition.lifecycle->on_config_changed(&rebound);
    require(constant_output == 0x40980000u,
            "an edited constant kept its previous output until interaction");

    /* 3.5 + 1.25 = 4.75 under the registered default (RNE). */
    const floatops::AddConfig add_default_config{32, 0, 0, {}};
    const uint64_t add_inputs[2] = {0x40600000u, 0x3FA00000u};
    uint64_t add_outputs[2] = {};
    invoke(add, TC_LOGIC_CYCLE, add_inputs, 2, add_outputs, 2, &add_default_config,
           sizeof(add_default_config), 3);
    require(add_outputs[0] == 0x40980000u && add_outputs[1] == 0,
            "3.5 + 1.25 did not come out as 4.75 with no flags");

    /* The rounding box: 1.0 + 2**-25 is exactly halfway, so the mode shows up in
       both the result and the flags. */
    const uint64_t halfway[2] = {0x3F800000u, 0x33000000u};
    const floatops::AddConfig rounding_configs[] = {{32, 2, 0, {}}, {32, 4, 0, {}}}; /* RTZ, RUP */
    const uint64_t expected[2] = {0x3F800000u, 0x3F800001u};
    for (int index = 0; index < 2; ++index) {
        uint64_t outputs[2] = {};
        invoke(add, TC_LOGIC_CYCLE, halfway, 2, outputs, 2, &rounding_configs[index],
               sizeof(floatops::AddConfig), 3);
        require(outputs[0] == expected[index] && outputs[1] == tcfp::fp_flag_nx,
                "the rounding mode in the configuration did not reach the kernel");
    }

    /* The display formats the value it was given, from the same cache the render
       callback reads. */
    const floatops::DisplayConfig display_config{32, 0, 0, {}};
    const uint64_t display_input[1] = {0x40980000u};
    invoke(display, TC_LOGIC_CYCLE, display_input, 1, nullptr, 0, &display_config,
           sizeof(display_config), 3);
    char shown[64] = {};
    floatops::displayText(UINT64_C(0x12345678), shown, sizeof(shown));
    require(std::strcmp(shown, "4.75 0x40980000") == 0,
            std::string("the display showed \"") + shown + "\"");
    invoke(display, TC_LOGIC_RESET, display_input, 1, nullptr, 0, &display_config,
           sizeof(display_config), 3);
    char cleared[64] = {};
    require(floatops::displayText(UINT64_C(0x12345678), cleared, sizeof(cleared)) == 0,
            "RESET must forget the displayed value");
    /* The offline pass/dual/sink probes are deliberately fed vectors that are
       not the M0 pattern, so only the fixed-value probes can assert the mismatch
       counter here: the flags sink always sees 0x1f and the source always
       publishes 0xdeadbeef.  Every reset line is emitted before any cycle of
       this test, so each one must report an empty segment. */
    int emptySegments = 0;
    for (const std::string& message : fake.messages) {
        const bool fixedValue = message.find("float-ops M0: source reset #") == 0 ||
                                message.find("float-ops M0: flags-sink reset #") == 0;
        if (!fixedValue) continue;
        require(message.find("previous cycle=0 refresh=0 mismatched=0") != std::string::npos,
                "a fixed-value probe reported a non-empty segment: " + message);
        ++emptySegments;
    }
    require(emptySegments >= 2, "missing reset reports for the fixed-value probes");

    /* ---- M3/M4: the catalogue, offline -----------------------------------
       Every catalogue type registers with the shape its table row declares, all
       of them share the one configuration layout, and the logic callbacks run
       the kernel entry point their type is for.  The arithmetic itself is
       covered by the kernel vectors; what this section proves is the plumbing
       the game sees: pins, widths, configuration and the published pins. */
    struct CatalogueCase {
        const char* type_id;
        uint32_t inputs;
        uint32_t outputs;
        const uint64_t* input_values;   /* inputs, in pin order */
        const floatops::OpsConfig config;
        const uint64_t* expected;       /* outputs, in pin order */
    };
    const uint64_t twoNumbers[2] = {0x40000000u, 0x40400000u};       /* 2.0, 3.0 */
    const uint64_t oneNumber[1] = {0x40490FDBu};
    const uint64_t relationInputs[2] = {0x7FC00000u, 0x3F800000u};   /* qNaN, 1.0 */
    const uint64_t splitInputs[1] = {0xBFC00000u};                   /* -1.5 */
    const uint64_t makeInputs[3] = {1u, 0x7Fu, 0x400000u};
    const uint64_t intInput[1] = {0xFFFFFFFFu};                      /* -1 */
    const uint64_t uintInput[1] = {0xFFFFFFFFu};

    const uint64_t subtractExpected[2] = {0xBF800000u, 0};           /* 2 - 3 */
    const uint64_t multiplyExpected[2] = {0x40C00000u, 0};           /* 2 * 3 */
    const uint64_t divideExpected[2] = {0x3F2AAAABu, tcfp::fp_flag_nx};
    const uint64_t sqrtExpected[2] = {0x3FE2DFC5u, tcfp::fp_flag_nx};
    const uint64_t negateExpected[1] = {0xC0490FDBu};
    const uint64_t absoluteExpected[1] = {0x40490FDBu};
    const uint64_t compareExpected[4] = {0, 0, 0, 0};                /* unordered, quiet */
    const uint64_t classifyExpected[1] = {1u << 9};                  /* quiet NaN */
    const uint64_t fmaExpected[2] = {0x41300000u, 0};                /* 2*3+5 */
    /* IEEE remainder of 5/3 is -1: the quotient 1.67 rounds to 2, so the result
       is 5 - 6, and that is exact (no flags). */
    const uint64_t remainderExpected[2] = {0xBF800000u, 0};          /* rem(5, 3) */
    const uint64_t roundExpected[2] = {0x40400000u, tcfp::fp_flag_nx}; /* round(3.14) */
    const uint64_t minimumExpected[2] = {0x40000000u, 0};
    const uint64_t maximumExpected[2] = {0x40400000u, 0};
    const uint64_t i32ToFloatExpected[2] = {0xBF800000u, 0};
    const uint64_t u32ToFloatExpected[2] = {0x4F800000u, tcfp::fp_flag_nx};
    const uint64_t floatToI32Expected[2] = {0x7FFFFFFFu, tcfp::fp_flag_nv};  /* NaN */
    const uint64_t floatToU32Expected[2] = {0xFFFFFFFFu, tcfp::fp_flag_nv};
    const uint64_t splitExpected[3] = {1u, 0x7Fu, 0x400000u};
    const uint64_t makeExpected[1] = {0xBFC00000u};

    const uint64_t fmaInputs[3] = {0x40000000u, 0x40400000u, 0x40A00000u}; /* 2*3+5 */
    const uint64_t remainderInputs[2] = {0x40A00000u, 0x40400000u};         /* 5, 3 */
    const uint64_t minmaxInputs[2] = {0x40000000u, 0x40400000u};

    const CatalogueCase cases[] = {
        {"local.float-ops/fp32-subtract", 2, 2, twoNumbers, {32, 0, 0, {}}, subtractExpected},
        {"local.float-ops/fp32-multiply", 2, 2, twoNumbers, {32, 0, 0, {}}, multiplyExpected},
        {"local.float-ops/fp32-divide", 2, 2, twoNumbers, {32, 0, 0, {}}, divideExpected},
        {"local.float-ops/fp32-square-root", 1, 2, oneNumber, {32, 0, 0, {}}, sqrtExpected},
        {"local.float-ops/fp32-negate", 1, 1, oneNumber, {32, 0, 0, {}}, negateExpected},
        {"local.float-ops/fp32-absolute", 1, 1, oneNumber, {32, 0, 0, {}}, absoluteExpected},
        {"local.float-ops/fp32-compare", 2, 4, relationInputs, {32, 0, 0, {}}, compareExpected},
        {"local.float-ops/fp32-classify", 1, 1, relationInputs, {32, 0, 0, {}}, classifyExpected},
        {"local.float-ops/fp32-fma", 3, 2, fmaInputs, {32, 0, 0, {}}, fmaExpected},
        {"local.float-ops/fp32-remainder", 2, 2, remainderInputs, {32, 4, 0, {}}, remainderExpected},
        {"local.float-ops/fp32-round-to-integral", 1, 2, oneNumber, {32, 0, 0, {}}, roundExpected},
        {"local.float-ops/fp32-minimum", 2, 2, minmaxInputs, {32, 0, 0, {}}, minimumExpected},
        {"local.float-ops/fp32-maximum", 2, 2, minmaxInputs, {32, 0, 0, {}}, maximumExpected},
        {"local.float-ops/i32-to-fp32", 1, 2, intInput, {32, 0, 0, {}}, i32ToFloatExpected},
        {"local.float-ops/u32-to-fp32", 1, 2, uintInput, {32, 0, 0, {}}, u32ToFloatExpected},
        {"local.float-ops/fp32-to-i32", 1, 2, relationInputs, {32, 0, 0, {}}, floatToI32Expected},
        {"local.float-ops/fp32-to-u32", 1, 2, relationInputs, {32, 0, 0, {}}, floatToU32Expected},
        {"local.float-ops/fp32-split-bits", 1, 3, splitInputs, {32, 0, 0, {}}, splitExpected},
        {"local.float-ops/fp32-make-bits", 3, 1, makeInputs, {32, 0, 0, {}}, makeExpected},
    };
    require(sizeof(cases) / sizeof(cases[0]) == floatops::catalogueCount(),
            "the catalogue case list and the Mod's own catalogue disagree");
    int roundingTypes = 0;
    for (const CatalogueCase& item : cases) {
        const auto& definition = findDefinition(fake, item.type_id);
        verifyShape(definition, item.inputs, item.outputs);
        require(definition.definition.config_size == sizeof(floatops::OpsConfig) &&
                    definition.definition.config_schema == 3 &&
                    definition.definition.default_config != nullptr,
                std::string(item.type_id) + " must carry the shared schema-3 "
                "configuration (rounding + label)");
        require(definition.definition.pin_lane == 3.0f,
                std::string(item.type_id) + " must put its pins on the 3.0 lane");
        if (floatops::catalogueInfo(definition.definition.custom_id)->rounding)
            ++roundingTypes;
        uint64_t outputs[4] = {};
        invoke(definition, TC_LOGIC_CYCLE, item.input_values, item.inputs, outputs,
               item.outputs, &item.config, sizeof(item.config), 3);
        for (uint32_t pin = 0; pin < item.outputs; ++pin)
            require(outputs[pin] == item.expected[pin],
                    std::string(item.type_id) + " pin " + std::to_string(pin) +
                        " came out as " + std::to_string(outputs[pin]));
    }
    require(roundingTypes == 10,
            "ten types take a rounding mode (the arithmetic and the conversions)");

    /* Rounding really is per instance for the catalogue too: 1.0 + 2**-25 is
       halfway, so RNE and RUP have to disagree through the Subtract type. */
    {
        const auto& subtract = findDefinition(fake, "local.float-ops/fp32-subtract");
        const uint64_t operands[2] = {0x3F800000u, 0xB3000000u};   /* 1.0 - 2**-25 */
        const floatops::OpsConfig modes[] = {{32, 0, 0, {}}, {32, 4, 0, {}}}; /* RNE, RUP */
        const uint64_t expected[2] = {0x3F800000u, 0x3F800001u};
        for (int index = 0; index < 2; ++index) {
            uint64_t outputs[2] = {};
            invoke(subtract, TC_LOGIC_CYCLE, operands, 2, outputs, 2, &modes[index],
                   sizeof(floatops::OpsConfig), 3);
            require(outputs[0] == expected[index] && outputs[1] == tcfp::fp_flag_nx,
                    "the instance's rounding mode did not reach the catalogue kernel");
        }
    }

    /* Split Bits and Make Bits round trip: the three fields of -1.5 rebuild the
       pattern bit for bit. */
    {
        const auto& split = findDefinition(fake, "local.float-ops/fp32-split-bits");
        const auto& make = findDefinition(fake, "local.float-ops/fp32-make-bits");
        const floatops::OpsConfig config{32, 0, 0, {}};
        uint64_t fields[3] = {};
        invoke(split, TC_LOGIC_CYCLE, splitInputs, 1, fields, 3, &config, sizeof(config), 3);
        uint64_t rebuilt[1] = {};
        invoke(make, TC_LOGIC_CYCLE, fields, 3, rebuilt, 1, &config, sizeof(config), 3);
        require(rebuilt[0] == splitInputs[0],
                "Split Bits followed by Make Bits did not rebuild the pattern");
    }

    /* The schema-2 migration path: a catalogue blob written without the label
       keeps its rounding mode and gets an empty label. */
    {
        const auto& multiply = findDefinition(fake, "local.float-ops/fp32-multiply");
        const floatops::OpsConfig older{32, 4, 0, {}};
        floatops::OpsConfig migrated{};
        const int status = multiply.definition.migrate_config(
            multiply.definition.migration_user, 2, &older, sizeof(older) - 16, 3,
            &migrated, sizeof(migrated));
        require(status == 0 && migrated.rounding == 4 && migrated.label[0] == 0,
                "the catalogue's schema-2 migration did not keep the rounding mode");
    }

    std::cout << "PASS float-ops M0/M2/M3/M4: five public V2 probes plus the three M2 "
                 "components and the catalogue; every catalogue type registers with its "
                 "declared pins, the shared schema-3 configuration and the 3.0 pin lane, "
                 "and its callback publishes the kernel's result and flags\n";
    return 0;
}
