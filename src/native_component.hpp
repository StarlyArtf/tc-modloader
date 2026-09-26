#pragma once
#include "component_definition.hpp"
#include "component_registry.hpp"
#include "../sdk/tc_component_model.h"
#include "native_logic.hpp"

namespace tc {
/* The catalogue entry a declarative definition describes, before the loader has
   decided whether it can be bridged. */
inline component_registry::Type declaredType(const TCHost* host,
                                             const TCNativeComponentDefinition* d) {
    component_registry::Type type;
    type.custom_id=d->custom_id;
    type.owner_mod=host&&host->mod_id?host->mod_id:"";
    type.name=d->name?d->name:"";
    type.description=d->description?d->description:"";
    type.gate_cost=d->gate_cost;
    type.delay=d->delay;
    for(unsigned i=0;i<d->input_count;++i)
        type.inputs.push_back(component_registry::Pin{d->inputs[i].name?d->inputs[i].name:"",
                                                      d->inputs[i].bits});
    for(unsigned i=0;i<d->output_count;++i)
        type.outputs.push_back(component_registry::Pin{d->outputs[i].name?d->outputs[i].name:"",
                                                       d->outputs[i].bits});
    return type;
}
inline int registerNativeComponent(const TCHost* host, const TCNativeComponentDefinition* d) {
    const auto refuse=[&](int code,const char* why) {
        /* The first reason wins: a later, more generic step must not hide the
           specific one, and the bridge's own answer ("up to eight pins per
           direction, ...") is the most useful of them. */
        component_registry::Type existing;
        if(!component_registry::find(d?d->custom_id:0,&existing))
            component_registry::noteRefused(d?d->custom_id:0,
                                            host&&host->mod_id?host->mod_id:"",
                                            d&&d->name?d->name:"",why);
        return code;
    };
    if (!logic::installed) return refuse(-1,"native logic is not installed in this game build");
    if (!component_definition::valid(d))
        return refuse(-2,"the definition is malformed (name, pins, widths or counts)");
    TCGameModel game; TCComponentModel components;
    if (!game.load(host) || !components.load(host) || components.readiness()!=TCComponentStatus::Ok)
        return refuse(-4,"the game's component model is unavailable");
    if (game.hasCustomPrototype(d->custom_id)) return refuse(-3,"that id is already imported");
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(d->custom_id)) return refuse(-3,"that id is already registered");
    }
    // Only this new ID can be removed on failure; duplicates returned above.
    struct Rollback {
        TCGameModel& game; uint64_t id; bool committed=false;
        ~Rollback() { if(!committed) game.removeCustomPrototype(id); }
    } rollback{game,d->custom_id};
    const auto bytes=component_definition::encode(*d);
    if (bytes.empty()) return refuse(-2,"the definition could not be encoded");
    std::string directory=std::string(host->data_directory_utf8)+"/";
    const auto imported=components.importCircuit(d->name,bytes.data(),bytes.size(),directory.c_str());
    if (!imported.ok() || imported.custom_id!=d->custom_id)
        return refuse(-4,"the game refused to import the generated definition");
    TCPrototype prototype{};
    if (!game.getCustomPrototype(d->custom_id,prototype))
        return refuse(-4,"the imported prototype could not be read back");
    bool ok=game.setPrototypeGateCost(prototype,d->gate_cost) && game.setPrototypeDelay(prototype,d->delay);
    if (d->description) ok=ok && game.setPrototypeDescription(prototype,d->description);
    if (d->shape_svg) ok=ok && game.setPrototypeShapeSvg(prototype,d->shape_svg);
    ok=ok && game.setCustomPrototype(d->custom_id,prototype);
    ok=components.releasePrototype(prototype)==TCComponentStatus::Ok && ok;
    if (!ok) return refuse(-4,"the prototype's statistics could not be written");
    TCLogicDefinition logicDefinition{sizeof(TCLogicDefinition),3,d->custom_id,d->callback,d->user};
    if (logic::add(&logicDefinition,host&&host->mod_id?host->mod_id:nullptr))
        return refuse(-5,"the shape could not be bridged to the callback");
    {
        component_registry::Type type=declaredType(host,d);
        type.active=true;
        component_registry::note(std::move(type));
    }
    rollback.committed=true;
    return 0;
}
/* V2 registration (tc.component.types): the shape may be wider than the V1
   callback struct (up to kMaxBridgePins per direction, still within the 128-bit
   input budget), the callback takes TCLogicIOV2, and the definition chooses its
   per-instance state size.  The generated scaffold and the bridge's emission
   path are the same, so this only differs in validation, the catalogue record
   and which callback the bridge calls. */
inline int registerNativeComponentV2(const TCHost* host,
                                     const TCComponentTypeDefinitionV2* d) {
    const auto refuse=[&](int code,const char* why) {
        component_registry::Type existing;
        if(!component_registry::find(d?d->custom_id:0,&existing))
            component_registry::noteRefused(d?d->custom_id:0,
                                            host&&host->mod_id?host->mod_id:"",
                                            d&&d->name?d->name:"",why);
        return code;
    };
    if (!logic::installed)
        return refuse(TC_COMPONENT_TYPES_ERR_UNAVAILABLE,
                      "native logic is not installed in this game build");
    const size_t requiredSize=offsetof(TCComponentTypeDefinitionV2,lifecycle);
    if (!d || d->size < requiredSize || d->version != TC_COMPONENT_TYPES_VERSION_2 ||
        !d->custom_id || !d->name || !*d->name || std::strlen(d->name) > 65535 ||
        !d->callback || d->input_count > logic::kMaxBridgePins ||
        d->output_count > logic::kMaxBridgePins ||
        (d->input_count != 0 && !d->inputs) || (d->output_count != 0 && !d->outputs))
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                      "the definition is malformed (name, pins, widths or counts)");
    uint32_t inputBits = 0;
    for (uint32_t i = 0; i < d->input_count; ++i) {
        const TCComponentPinV2& pin = d->inputs[i];
        if (pin.bits < 1 || pin.bits > 64)
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT, "a pin's width is not 1..64 bits");
        inputBits += pin.bits;
    }
    for (uint32_t i = 0; i < d->output_count; ++i) {
        const TCComponentPinV2& pin = d->outputs[i];
        if (pin.bits < 1 || pin.bits > 64)
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT, "a pin's width is not 1..64 bits");
    }
    /* The generated call carries two 64-bit payload words and nothing wider
       survives the JIT, so the input budget is a host limit, not a style rule. */
    if (inputBits > logic::kMaxInputBits)
        return refuse(TC_COMPONENT_TYPES_ERR_BUDGET,
                      "the declared input pins add up to more than 128 bits, which is the "
                      "widest payload the generated call can carry");

    /* The encoder works on the shape, which is the V1 descriptor minus the
       callback: pin names and widths only. */
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(d->input_count) + d->output_count);
    std::vector<TCComponentPin> ins, outs;
    const auto addPins=[&](const TCComponentPinV2* pins,uint32_t count,
                           std::vector<TCComponentPin>& out) {
        for (uint32_t i = 0; i < count; ++i) {
            const char* name = pins[i].name && *pins[i].name ? pins[i].name
                              : (pins[i].pin_id && *pins[i].pin_id ? pins[i].pin_id : "");
            names.emplace_back(name);
            out.push_back({names.back().c_str(), pins[i].bits});
        }
    };
    addPins(d->inputs, d->input_count, ins);
    addPins(d->outputs, d->output_count, outs);
    TCNativeComponentDefinition shaped{};
    shaped.size = sizeof(shaped);
    shaped.custom_id = d->custom_id;
    shaped.name = d->name;
    shaped.description = d->description;
    shaped.shape_svg = d->shape_svg;
    shaped.input_count = d->input_count;
    shaped.output_count = d->output_count;
    shaped.inputs = ins.empty() ? nullptr : ins.data();
    shaped.outputs = outs.empty() ? nullptr : outs.data();
    shaped.gate_cost = d->gate_cost;
    shaped.delay = d->delay;
    if (!component_definition::validShape(&shaped, logic::kMaxBridgePins))
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT, "the definition is malformed (name, pins, widths or counts)");

    /* The pin lane was appended to the definition after the migration tail: a
       caller whose `size` stops before it keeps the historical lane of 2.0
       cells, and a caller that asks for one gets the generated pins moved
       outwards (the wires follow the pins, so the type still compiles and
       runs).  Out-of-range lanes are refused rather than clamped: a pin at 0 or
       at 1000 cells is a typo, and silently parking a type's pins somewhere
       else would be worse than not registering it. */
    const size_t pinLaneTail=offsetof(TCComponentTypeDefinitionV2,pin_lane)+
                             sizeof(d->pin_lane);
    float pinLane=component_definition::kDefaultPinLane;
    if (d->size >= pinLaneTail && d->pin_lane != 0.f) {
        if (!component_definition::validPinLane(d->pin_lane))
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                          "pin_lane must be 1.0 to 16.0 board cells (0 keeps the default of 2.0)");
        pinLane=d->pin_lane;
    }

    TCGameModel game; TCComponentModel components;
    if (!game.load(host) || !components.load(host) || components.readiness()!=TCComponentStatus::Ok)
        return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the game's component model is unavailable");
    if (game.hasCustomPrototype(d->custom_id))
        return refuse(TC_COMPONENT_TYPES_ERR_DUPLICATE, "that id is already imported");
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(d->custom_id))
            return refuse(TC_COMPONENT_TYPES_ERR_DUPLICATE, "that id is already registered");
    }
    struct Rollback {
        TCGameModel& game; uint64_t id; bool committed=false;
        ~Rollback() { if(!committed) game.removeCustomPrototype(id); }
    } rollback{game,d->custom_id};
    const auto bytes=component_definition::encode(shaped,logic::kMaxBridgePins,pinLane);
    if (bytes.empty())
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT, "the definition could not be encoded");
    std::string directory=std::string(host->data_directory_utf8)+"/";
    const auto imported=components.importCircuit(d->name,bytes.data(),bytes.size(),directory.c_str());
    if (!imported.ok() || imported.custom_id!=d->custom_id)
        return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the game refused to import the generated definition");
    TCPrototype prototype{};
    if (!game.getCustomPrototype(d->custom_id,prototype))
        return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the imported prototype could not be read back");
    bool ok=game.setPrototypeGateCost(prototype,d->gate_cost) && game.setPrototypeDelay(prototype,d->delay);
    if (d->description) ok=ok && game.setPrototypeDescription(prototype,d->description);
    if (d->shape_svg) ok=ok && game.setPrototypeShapeSvg(prototype,d->shape_svg);
    ok=ok && game.setCustomPrototype(d->custom_id,prototype);
    ok=components.releasePrototype(prototype)==TCComponentStatus::Ok && ok;
    if (!ok) return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the prototype's statistics could not be written");
    logic::V2Registration registration;
    registration.custom_id=d->custom_id;
    registration.callback=d->callback;
    registration.user=d->user;
    registration.stateWords=d->state_words;
    registration.owner_mod=host&&host->mod_id?host->mod_id:nullptr;
    /* The lifecycle pointer was appended to the definition after its first
       release: a caller whose `size` stops before it simply gets no lifecycle
       notifications, and we must not read past `d->size`. */
    if (d->size >= offsetof(TCComponentTypeDefinitionV2,lifecycle)+sizeof(d->lifecycle))
        registration.lifecycle=d->lifecycle;
    const size_t configTail=offsetof(TCComponentTypeDefinitionV2,default_config)+
                            sizeof(d->default_config);
    if (d->size >= configTail) {
        if (d->config_size > logic::kMaxConfigBytes ||
            (d->config_size != 0 && !d->default_config))
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                          "configuration must be at most 65536 bytes and have a default blob");
        registration.configSchema=d->config_schema;
        if(d->config_size) {
            const auto* first=static_cast<const uint8_t*>(d->default_config);
            registration.defaultConfig.assign(first,first+d->config_size);
        }
    }
    /* The migration tail was appended after the configuration tail: a caller
       whose `size` stops before it simply has no migration and gets the
       conservative behaviour (keep the stored record, run on the default). */
    const size_t migrationTail=offsetof(TCComponentTypeDefinitionV2,migration_user)+
                               sizeof(d->migration_user);
    if (d->size >= migrationTail) {
        if (d->config_migration_version == TC_COMPONENT_CONFIG_MIGRATION_VERSION_1 &&
            d->migrate_config) {
            registration.migrationVersion=d->config_migration_version;
            registration.migrateConfig=d->migrate_config;
            registration.migrationUser=d->migration_user;
        } else if (d->config_migration_version) {
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                          "config_migration_version is set but no migration callback is provided, "
                          "or the version is not one this loader knows");
        }
    }
    const int bridged=logic::addV2(registration);
    if (bridged != 0)
        return refuse(bridged==-1?TC_COMPONENT_TYPES_ERR_UNAVAILABLE:TC_COMPONENT_TYPES_ERR_UNSUPPORTED,
                      "the shape could not be bridged to the callback");
    {
        component_registry::Type type;
        type.custom_id=d->custom_id;
        type.owner_mod=host&&host->mod_id?host->mod_id:"";
        type.name=d->name;
        type.description=d->description?d->description:"";
        type.gate_cost=d->gate_cost;
        type.delay=d->delay;
        /* Where this type's pins ended up, so the geometry warning can measure a
           footprint against the lane the type really has instead of assuming
           the default one (see component_geometry::footprintReachesPins). */
        type.pin_lane=pinLane;
        for (uint32_t i=0;i<d->input_count;++i) {
            const char* label=d->inputs[i].pin_id&&*d->inputs[i].pin_id?d->inputs[i].pin_id
                              :(d->inputs[i].name?d->inputs[i].name:"");
            type.inputs.push_back(component_registry::Pin{label,d->inputs[i].bits});
        }
        for (uint32_t i=0;i<d->output_count;++i) {
            const char* label=d->outputs[i].pin_id&&*d->outputs[i].pin_id?d->outputs[i].pin_id
                              :(d->outputs[i].name?d->outputs[i].name:"");
            type.outputs.push_back(component_registry::Pin{label,d->outputs[i].bits});
        }
        type.active=true;
        component_registry::note(std::move(type));
    }
    rollback.committed=true;
    return 0;
}
}
