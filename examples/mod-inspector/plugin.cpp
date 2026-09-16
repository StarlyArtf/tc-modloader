#include "../../sdk/tc_mod.h"
#include <string>

struct V2 { float x, y; };
static const TCHost* host;
static tc::TCMod model;

template <class T> static T api(const char* name) {
    void* p = host->engine_proc(host->context, name);
    T f{};
    memcpy(&f, &p, sizeof(f));
    return f;
}

static void text(const std::string& s) {
    api<void (*)(const char*, const char*)>("igTextUnformatted")(s.c_str(), nullptr);
}

static void frame(void*, const TCFrame*) {
    if (!model.valid()) return;
    float w = 0;
    auto vp = static_cast<float*>(api<void* (*)()>("igGetMainViewport")());
    if (vp) w = vp[4];
    api<void (*)(V2, int)>("igSetNextWindowSize")({300, 0}, 1);
    api<void (*)(V2, int)>("igSetNextWindowPos")({w - 320, 12}, 1);
    bool open = true;
    if (api<bool (*)(const char*, bool*, int)>("igBegin")("TC Mod Inspector###TCModInspector", &open, 1 | 2 | 4 | 8 | 32 | 64 | 256)) {
        api<void (*)(float)>("igSetWindowFontScale")(0.6f);
        text("campaign: " + std::string(model.state.isCampaign() ? "yes" : "no"));
        text("campaign name: " + std::string(model.state.campaignNameCStr() ? model.state.campaignNameCStr() : ""));
        text("save count: " + std::to_string(model.save.saveCount()));
        text("cycle: " + std::to_string(model.simulation.cycle()));
        text("built-in prototypes: " + std::to_string(model.game.builtinPrototypeCount()));
        text("custom prototypes: " + std::to_string(model.game.customPrototypeCount()));
        text("selected components: " + std::to_string(model.board.selectedComponentCount()));
        text("selected wires: " + std::to_string(model.board.selectedWireCount()));
        text("level path: " + std::string(model.save.levelPathCStr() ? model.save.levelPathCStr() : ""));
    }
    api<void (*)()>("igEnd")();
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < sizeof(TCHost) ||
        !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!model.load(h)) return 2;
    for (auto name : {"igGetMainViewport", "igBegin", "igEnd", "igSetNextWindowSize", "igSetNextWindowPos", "igSetWindowFontScale", "igTextUnformatted"}) {
        if (!h->engine_proc(h->context, name)) return 3;
    }
    out->on_frame = frame;
    h->log(h->context, "TC Mod Inspector loaded");
    return 0;
}
