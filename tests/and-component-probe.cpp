// Development-only real-game validation for the generated two-input AND
// component fixture. Never package as a player mod.
#include "../sdk/tc_mod.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
const TCHost* host;
tc::TCMod mod;
bool done;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

}  // namespace

static void frame(void*, const TCFrame*) {
    if (done) return;
    done = true;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& value) {
        log(value);
        std::ofstream(folder / "result.txt") << value;
    };

    std::ifstream file(folder / "fixtures" / "and2_component.data",
                       std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) {
        finish("FAIL AND fixture missing");
        return;
    }
    const auto directory = (folder / "fixtures").generic_u8string() + "/";
    auto imported = mod.components.importCircuit(
        "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
    log("AND import status=" + std::to_string(static_cast<int>(imported.status)) +
        " id=" + std::to_string(imported.custom_id));
    if (!imported.ok() || imported.custom_id != kAndComponentId ||
        !mod.game.hasCustomPrototype(kAndComponentId)) {
        finish("FAIL AND import or identity");
        return;
    }

    tc::TCPrototype prototype{};
    mod.game.getCustomPrototype(kAndComponentId, prototype);
    const char* name = tc::prototypeNameCStr(prototype);
    const bool name_ok =
        name != nullptr && std::string(name) == "AND2 Test";
    const bool shape_ok =
        tc::prototypeInputCount(prototype) == 2 &&
        tc::prototypeOutputCount(prototype) == 1;
    bool width_ok = shape_ok;
    for (uint64_t i = 0; i < tc::prototypeInputCount(prototype); ++i) {
        auto* pin = tc::prototypeInputPin(prototype, i);
        width_ok = width_ok && pin != nullptr &&
                   tc::pinWordSizeRaw(*pin) == 1;
    }
    for (uint64_t i = 0; i < tc::prototypeOutputCount(prototype); ++i) {
        auto* pin = tc::prototypeOutputPin(prototype, i);
        width_ok = width_ok && pin != nullptr &&
                   tc::pinWordSizeRaw(*pin) == 1;
    }
    log("AND shape name=" + std::string(name ? name : "<null>") +
        " inputs=" + std::to_string(tc::prototypeInputCount(prototype)) +
        " outputs=" + std::to_string(tc::prototypeOutputCount(prototype)) +
        " one_bit_pins=" + std::to_string(width_ok));
    if (!name_ok || !shape_ok || !width_ok ||
        mod.components.releasePrototype(prototype) !=
            tc::TCComponentStatus::Ok ||
        mod.components.readiness() != tc::TCComponentStatus::Ok) {
        finish("FAIL AND name, shape, width or snapshot release");
        return;
    }
    finish("PASS real-game AND import: 64-bit id, two one-bit inputs, "
           "one one-bit output, name and owned snapshot release");
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;
    plugin->on_frame = frame;
    log("AND component probe ready");
    return 0;
}
