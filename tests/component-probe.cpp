// Development-only real-game probe. Never package as a player mod.
#include "../sdk/tc_mod.h"
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>

static const TCHost* host;
static tc::TCGameModel game;
static tc::TCComponentModel components;
static bool done;
static void log(const std::string& s) { host->log(host->context, s.c_str()); }
static void frame(void*, const TCFrame*) {
    if (done) return;
    done = true;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& value) {
        log(value);
        std::ofstream(folder / "result.txt") << value;
    };
    std::ifstream file(folder / "fixture" / "circuit.data", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) { finish("FAIL fixture missing"); return; }
    const auto dir = (folder / "fixture").generic_u8string() + "/";
    tc::TCPrototype textOnly{};
    if (!game.setPrototypeName(textOnly, "Length check") ||
        tc::prototypeName(textOnly).length != 12 ||
        components.releasePrototype(textOnly) != tc::TCComponentStatus::Ok) {
        finish("FAIL real allocator string length"); return;
    }
    auto a = components.importCircuit("Probe component", bytes.data(), bytes.size(), dir.c_str());
    log("import status=" + std::to_string((int)a.status) + " id=" + std::to_string(a.custom_id));
    if (!a.ok() || !game.hasCustomPrototype(a.custom_id)) { finish("FAIL import"); return; }
    auto count = game.customPrototypeCount();
    tc::TCPrototype p{};
    game.getCustomPrototype(a.custom_id, p);
    bool nameOk = tc::prototypeName(p).length == 15 &&
        std::string(tc::prototypeNameCStr(p)) == "Probe component";
    log("snapshot inputs=" + std::to_string(tc::prototypeInputCount(p)) +
        " outputs=" + std::to_string(tc::prototypeOutputCount(p)));
    if (!nameOk || components.releasePrototype(p) != tc::TCComponentStatus::Ok) {
        finish("FAIL snapshot name or release"); return;
    }
    auto b = components.updateFromDirectory(dir.c_str(), "Renamed component");
    if (!b.ok() || b.custom_id != a.custom_id || game.customPrototypeCount() != count) {
        finish("FAIL update"); return;
    }
    game.getCustomPrototype(b.custom_id, p);
    nameOk = tc::prototypeName(p).length == 17 &&
        std::string(tc::prototypeNameCStr(p)) == "Renamed component";
    if (!nameOk || components.releasePrototype(p) != tc::TCComponentStatus::Ok) {
        finish("FAIL updated snapshot"); return;
    }
    auto missing = components.updateFromDirectory((dir + "missing/").c_str(), "Missing");
    if (missing.status != tc::TCComponentStatus::Rejected ||
        components.readiness() != tc::TCComponentStatus::Ok) {
        finish("FAIL missing directory handling"); return;
    }
    finish("PASS real-game string allocation, circuit import, name, same-ID update, owned snapshot release and missing-directory rejection");
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!components.load(h) || !game.load(h) || !game.valid()) return 2;
    out->on_frame = frame;
    log("Component probe ready");
    return 0;
}
