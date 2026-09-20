#pragma once
#include "component_definition.hpp"
#include "../sdk/tc_component_model.h"
#include "native_logic.hpp"

namespace tc {
inline int registerNativeComponent(const TCHost* host, const TCNativeComponentDefinition* d) {
    if (!logic::installed) return -1;
    if (!component_definition::valid(d)) return -2;
    TCGameModel game; TCComponentModel components;
    if (!game.load(host) || !components.load(host) || components.readiness()!=TCComponentStatus::Ok) return -4;
    if (game.hasCustomPrototype(d->custom_id)) return -3;
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(d->custom_id)) return -3;
    }
    // Only this new ID can be removed on failure; duplicates returned above.
    struct Rollback {
        TCGameModel& game; uint64_t id; bool committed=false;
        ~Rollback() { if(!committed) game.removeCustomPrototype(id); }
    } rollback{game,d->custom_id};
    const auto bytes=component_definition::encode(*d);
    std::string directory=std::string(host->data_directory_utf8)+"/";
    const auto imported=components.importCircuit(d->name,bytes.data(),bytes.size(),directory.c_str());
    if (!imported.ok() || imported.custom_id!=d->custom_id) return -4;
    TCPrototype prototype{};
    if (!game.getCustomPrototype(d->custom_id,prototype)) return -4;
    bool ok=game.setPrototypeGateCost(prototype,d->gate_cost) && game.setPrototypeDelay(prototype,d->delay);
    if (d->description) ok=ok && game.setPrototypeDescription(prototype,d->description);
    if (d->shape_svg) ok=ok && game.setPrototypeShapeSvg(prototype,d->shape_svg);
    ok=ok && game.setCustomPrototype(d->custom_id,prototype);
    ok=components.releasePrototype(prototype)==TCComponentStatus::Ok && ok;
    if (!ok) return -4;
    TCLogicDefinition logicDefinition{sizeof(TCLogicDefinition),3,d->custom_id,d->callback,d->user};
    if (logic::add(&logicDefinition)) return -5;
    rollback.committed=true;
    return 0;
}
}
