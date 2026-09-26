#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_mod.h"
#include "../../sdk/tc_component_registry.h"
#include <string>

static const TCHost* host;
static tc::TCMod model;
static bool show = true;
static bool reported = false;
static TCComponentRegistryApiV1 registry{};

static std::string number(const char* label, long long value) {
    return std::string(label) + std::to_string(value);
}

static void frame(void*, const TCFrame*) {
    if (!model.valid()) return;
    tc::ui::toggleHotkey(VK_F7, &show);

    // Panel geometry comes from the viewport, not from a local struct guess.
    const tc::ui::Vec2 position = tc::ui::topRight(320.f);
    if (auto panel = tc::ui::panel("TC Mod Inspector###TCModInspector", &show,
                                   {300, 0}, position, 0.6f, tc::ui::kToolbarFlags,
                                   tc::ui::Cond_Always)) {
        tc::ui::text(std::string("campaign: ") + (model.state.isCampaign() ? "yes" : "no"));
        tc::ui::text(std::string("campaign name: ") +
                     (model.state.campaignNameCStr() ? model.state.campaignNameCStr() : ""));
        tc::ui::text(number("save count: ", model.save.saveCount()));
        tc::ui::text(number("cycle: ", model.simulation.cycle()));
        tc::ui::text(number("built-in prototypes: ", model.game.builtinPrototypeCount()));
        tc::ui::text(number("custom prototypes: ", model.game.customPrototypeCount()));
        tc::ui::text(number("selected components: ", model.board.selectedComponentCount()));
        tc::ui::text(number("selected wires: ", model.board.selectedWireCount()));
        tc::ui::text(std::string("level path: ") +
                     (model.save.levelPathCStr() ? model.save.levelPathCStr() : ""));
        const tc::ui::Vec2 mouse = tc::ui::mousePos();
        tc::ui::text("mouse: " + std::to_string(static_cast<int>(mouse.x)) + ", " +
                     std::to_string(static_cast<int>(mouse.y)));
        tc::ui::textDisabled("F7 显示 / 隐藏");

        /* The component catalogue: every type this session registered, with the
           shape the game compiled and the reason a registration was refused.
           A Mod author reads this instead of guessing why a component is
           missing from the editor. */
        if (tc::component_registry::ready(registry)) {
            const uint32_t total = tc::component_registry::count(registry);
            tc::ui::separator();
            tc::ui::text("component types: " + std::to_string(total));
            for (uint32_t index = 0; index < total; ++index) {
                TCComponentTypeInfoV1 type = tc::component_registry::typeInfo();
                if (tc::component_registry::get(registry, index, &type) !=
                    TC_COMPONENT_REGISTRY_OK)
                    continue;
                std::string line = std::string(type.name) + " (" +
                                   std::to_string(type.input_count) + "→" +
                                   std::to_string(type.output_count) + " " +
                                   std::to_string(type.gate_cost) + "g/" +
                                   std::to_string(type.delay) + "d)";
                if (type.active) {
                    tc::ui::text(line);
                } else {
                    /* Refused types stay visible, with what went wrong. */
                    tc::ui::textDisabled(line + " — " + type.status);
                }
            }
        }

        // One line of evidence per run that the viewport and mouse entry points
        // returned plausible values (a wrong ABI shows up here as nonsense).
        if (!reported) {
            reported = true;
            const tc::ui::Vec2 viewport = tc::ui::viewportSize();
            host->log(host->context,
                      ("Mod Inspector: panel drawn; viewport=" +
                       std::to_string(static_cast<int>(viewport.x)) + "x" +
                       std::to_string(static_cast<int>(viewport.y)) + " mouse=" +
                       std::to_string(static_cast<int>(mouse.x)) + "," +
                       std::to_string(static_cast<int>(mouse.y)) + " frame=" +
                       std::to_string(tc::ui::frameCount())).c_str());
        }
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE ||
        !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!model.load(h)) return 2;
    if (!tc::ui::load(h)) return 3;  // missing exports are named in the log
    /* Optional: an older loader without the service leaves the section out. */
    tc::component_registry::table(h, &registry);
    out->on_frame = frame;
    h->log(h->context, "TC Mod Inspector loaded");
    return 0;
}
