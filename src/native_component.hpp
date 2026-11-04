#pragma once
#include "component_definition.hpp"
#include "component_netlist.hpp"
#include "component_registry.hpp"
#include "../sdk/tc_component_model.h"
#include "native_logic.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

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
/* Import a generated definition, or replace in place the prototype the game
   already holds for that id.

   The loader hands each registered type's definition to the foundry
   (`provideFoundrySchematicBytes`), and the game imports everything under
   `<profile>/schematics/foundry/` as a *player* component when it boots - before
   any plugin loads.  From then on a plain import of that id is refused as a
   duplicate for the rest of the session.  Deleting the visible prototype entry
   and adding it again does not work either: the game keeps custom ids in more
   than one index, so `add_custom_prototype` still reports the duplicate.
   `update_custom_prototype` replaces every index that backs the palette and the
   compiled-board path, which is exactly what re-registering the same type wants.

   Returns false with *why set when the definition could not be written at all;
   the caller still has to check `imported->ok()` and the id it came back with. */
inline bool importGeneratedDefinition(TCComponentModel& components, const TCHost* host,
                                      const std::vector<uint8_t>& bytes, const char* name,
                                      uint64_t customId, bool replaceExisting,
                                      TCComponentImportResult* imported, const char** why) {
    if (!replaceExisting) {
        std::string directory = std::string(host->data_directory_utf8) + "/";
        *imported = components.importCircuit(name, bytes.data(), bytes.size(),
                                             directory.c_str());
        return true;
    }
    if (!host || !host->data_directory_utf8 || !*host->data_directory_utf8) {
        *why = "the runtime update directory is unavailable";
        return false;
    }
    const std::filesystem::path scratch =
        std::filesystem::u8path(host->data_directory_utf8) / "tc-runtime-update";
    std::error_code directoryError;
    std::filesystem::create_directories(scratch, directoryError);
    if (directoryError) {
        *why = "the runtime update directory could not be created";
        return false;
    }
    {
        std::ofstream output(scratch / "circuit.data", std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            *why = "the runtime update definition could not be written";
            return false;
        }
    }
    std::string directory = scratch.generic_u8string();
    if (directory.empty() || directory.back() != '/') directory.push_back('/');
    *imported = components.updateFromDirectory(directory.c_str(), name);
    (void)customId;
    return true;
}
/* `encoded` (optional) hands the generated definition back to the caller: the
   same bytes are what the game reads as a player component, so the loader can
   also drop them into the foundry so the type can be opened and looked at. */
inline int registerNativeComponent(const TCHost* host, const TCNativeComponentDefinition* d,
                                   std::vector<uint8_t>* encoded = nullptr,
                                   bool adoptFoundryEntry = false) {
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
    const bool prototypeExists=game.hasCustomPrototype(d->custom_id);
    const bool replaceExisting=adoptFoundryEntry&&prototypeExists;
    if (prototypeExists&&!replaceExisting)
        return refuse(-3,"that id is already imported");
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(d->custom_id)) return refuse(-3,"that id is already registered");
    }
    // Only an ID this call imported can be removed on failure.
    struct Rollback {
        TCGameModel& game; uint64_t id; bool committed=false;
        ~Rollback() { if(!committed) game.removeCustomPrototype(id); }
    } rollback{game,d->custom_id};
    const auto bytes=component_definition::encode(*d);
    if (bytes.empty()) return refuse(-2,"the definition could not be encoded");
    if (encoded) *encoded = bytes;
    if (replaceExisting) rollback.committed=true; /* the prototype is the game's already */
    TCComponentImportResult imported{};
    const char* importWhy=nullptr;
    if(!importGeneratedDefinition(components,host,bytes,d->name,d->custom_id,replaceExisting,
                                  &imported,&importWhy))
        return refuse(-4,importWhy?importWhy:"the definition could not be written");
    if (!imported.ok() || imported.custom_id!=d->custom_id)
        return refuse(-4,replaceExisting?"the game refused to update the generated definition"
                                        :"the game refused to import the generated definition");
    TCPrototype prototype{};
    if (!game.getCustomPrototype(d->custom_id,prototype))
        return refuse(-4,"the imported prototype could not be read back");
    bool ok=game.setPrototypeGateCost(prototype,d->gate_cost) && game.setPrototypeDelay(prototype,d->delay);
    /* The legacy V1 table has no flags field and its layout cannot grow (a
       shorter `size` is refused), so it keeps the historical behaviour: the
       description it declares is published.  New types go through
       tc.component.types and opt in with TC_COMPONENT_TYPE_SHOW_DESCRIPTION. */
    if (d->description) {
        ok=ok && game.setPrototypeDescription(prototype,d->description);
        if (host&&host->log) {
            char line[160]{};
            std::snprintf(line,sizeof(line),
                          "Component V1: type 0x%llx publishes its description (the legacy "
                          "table has no show/hide flag)",
                          static_cast<unsigned long long>(d->custom_id));
            host->log(host->context,line);
        }
    }
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
    return 0;
}
/* A composite type (`tc.component.compose` V2): the definition *is* the inner
   circuit, so nothing is bridged - no callback, no collectors, no driver.  The
   game imports the netlist and compiles it like a hand-drawn board, which is why
   the gate count is read back rather than written: the number the game recomputed
   from the nodes is the proof it walked the netlist.  The declared delay *is*
   written, because the parser trusts that field verbatim (measured; see the
   fixture generator's header). */
inline int registerCompositeComponent(const TCHost* host,
                                      const component_netlist::Definition& definition,
                                      std::string& why,
                                      bool adoptFoundryEntry = false) {
    if (!logic::installed) { why = "native logic is not installed in this game build"; return -1; }
    TCGameModel game; TCComponentModel components;
    if (!game.load(host) || !components.load(host) ||
        components.readiness() != TCComponentStatus::Ok) {
        why = "the game's component model is unavailable";
        return -4;
    }
    const bool prototypeExists=game.hasCustomPrototype(definition.customId);
    const bool replaceExisting=adoptFoundryEntry&&prototypeExists;
    if (prototypeExists&&!replaceExisting) {
        why = "that id is already imported";
        return -3;
    }
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(definition.customId)) {
            why = "that id is already registered";
            return -3;
        }
    }
    struct Rollback {
        TCGameModel& game; uint64_t id; bool committed = false;
        ~Rollback() { if (!committed) game.removeCustomPrototype(id); }
    } rollback{game, definition.customId};
    const std::vector<uint8_t> bytes = component_netlist::encode(definition);
    if (bytes.empty()) { why = "the netlist could not be encoded"; return -2; }
    /* The composite's foundry entry is filled below, so from the next start-up on
       the game imports this id itself - replace that prototype in place. */
    if (replaceExisting) rollback.committed = true; /* the prototype is the game's already */
    TCComponentImportResult imported{};
    const char* importWhy = nullptr;
    if (!importGeneratedDefinition(components, host, bytes, definition.name.c_str(),
                                   definition.customId, replaceExisting, &imported, &importWhy)) {
        why = importWhy ? importWhy : "the composite definition could not be written";
        return -4;
    }
    if (!imported.ok() || imported.custom_id != definition.customId) {
        why = replaceExisting ? "the game refused to update the composite definition"
                              : "the game refused to import the composite definition";
        return -4;
    }
    TCPrototype prototype{};
    if (!game.getCustomPrototype(definition.customId, prototype)) {
        why = "the imported prototype could not be read back";
        return -4;
    }
    bool ok = game.setPrototypeDelay(prototype, static_cast<uint64_t>(definition.delay));
    ok = game.setCustomPrototype(definition.customId, prototype) && ok;
    ok = components.releasePrototype(prototype) == TCComponentStatus::Ok && ok;
    if (!ok) { why = "the prototype could not be written back"; return -4; }
    rollback.committed = true;
    if (host && host->log) {
        char line[240]{};
        std::snprintf(line, sizeof(line),
                      "Component compose: composite 0x%llx imported from %u node(s), "
                      "%u in/%u out, declared delay=%lld",
                      static_cast<unsigned long long>(definition.customId),
                      static_cast<unsigned>(definition.nodes.size()),
                      static_cast<unsigned>(definition.inputs.size()),
                      static_cast<unsigned>(definition.outputs.size()),
                      static_cast<long long>(definition.delay));
        host->log(host->context, line);
    }
    return 0;
}
/* V2 registration (tc.component.types): the shape may be wider than the V1
   callback struct (up to kMaxBridgePins per direction, within the 2048-bit
   staged-input budget), the callback takes TCLogicIOV2, and the definition chooses its
   per-instance state size.  The generated scaffold and the bridge's emission
   path are the same, so this only differs in validation, the catalogue record
   and which callback the bridge calls. */
inline int registerNativeComponentV2(const TCHost* host,
                                     const TCComponentTypeDefinitionV2* d,
                                     TCLogicCallbackV3 callbackV3=nullptr,
                                     std::vector<uint8_t>* encoded=nullptr,
                                     bool updateExisting=false,
                                     bool adoptFoundryEntry=false) {
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
        (!d->callback&&!callbackV3) || d->input_count > logic::kMaxBridgePins ||
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
    /* The JIT call stays four arguments wide; larger inputs are transferred by
       a bounded sequence of 128-bit staging calls before the callback fires. */
    if (inputBits > logic::kMaxInputBits)
        return refuse(TC_COMPONENT_TYPES_ERR_BUDGET,
                      "the declared input pins add up to more than 2048 bits, which is the "
                      "widest staged input batch the bridge accepts");

    const size_t flagsTail=offsetof(TCComponentTypeDefinitionV2,flags)+sizeof(d->flags);
    const uint32_t declaredFlags=d->size>=flagsTail?d->flags:0u;
    const size_t dependencyTail=offsetof(TCComponentTypeDefinitionV2,output_dependency_count)+
                                sizeof(d->output_dependency_count);
    const bool preciseDependencies=
        (declaredFlags&TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES)!=0;
    const bool clockedTick=(declaredFlags&TC_COMPONENT_TYPE_CLOCKED_TICK)!=0;
    const bool groupedOutputs=(declaredFlags&TC_COMPONENT_TYPE_GROUPED_OUTPUTS)!=0;
    const bool inlineExpressions=(declaredFlags&TC_COMPONENT_TYPE_INLINE_EXPRESSIONS)!=0;
    const uint32_t* outputDependencies=nullptr;
    if(clockedTick&&!preciseDependencies)
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                      "a clocked tick requires exact output dependencies");
    if(groupedOutputs&&(!preciseDependencies||!callbackV3))
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                      "grouped outputs require exact dependencies and a V3 callback");
    const size_t inlineTail=offsetof(TCComponentTypeDefinitionV2,inline_expressions)+
                            sizeof(d->inline_expressions);
    if(inlineExpressions) {
        if(d->size<inlineTail||
           d->inline_expression_version!=TC_COMPONENT_INLINE_EXPRESSIONS_VERSION_1||
           d->inline_expression_count!=d->output_count||!d->inline_expressions||
           d->state_words!=0||clockedTick||!preciseDependencies)
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                          "inline expressions require exact dependencies and a stateless "
                          "output program for every pin");
        size_t programBytes=0;
        for(uint32_t i=0;i<d->output_count;++i) {
            if(!d->inline_expressions[i])
                return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                              "an inline output expression is null");
            programBytes+=std::strlen(d->inline_expressions[i]);
            if(d->inline_prefixes&&d->inline_prefixes[i])
                programBytes+=std::strlen(d->inline_prefixes[i]);
        }
        if(programBytes>8u*1024u*1024u)
            return refuse(TC_COMPONENT_TYPES_ERR_BUDGET,
                          "inline output programs exceed the 8 MiB registration budget");
    }
    if(clockedTick&&(!d->input_count||!d->output_count))
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                      "a clocked tick requires at least one input and one output");
    if (preciseDependencies) {
        if (d->size<dependencyTail || d->output_dependency_count!=d->output_count ||
            (d->output_count&&!d->output_dependencies))
            return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                          "exact output dependencies need one input mask per output");
        const uint32_t validInputs=d->input_count>=32?~uint32_t(0):
            ((uint32_t(1)<<d->input_count)-1);
        for(uint32_t i=0;i<d->output_count;++i)
            if(d->output_dependencies[i]&~validInputs)
                return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT,
                              "an output dependency mask names an input that does not exist");
        outputDependencies=d->output_dependencies;
    }

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
    const bool prototypeExists=game.hasCustomPrototype(d->custom_id);
    const bool replaceExisting=updateExisting||(adoptFoundryEntry&&prototypeExists);
    if (!updateExisting&&prototypeExists&&!replaceExisting)
        return refuse(TC_COMPONENT_TYPES_ERR_DUPLICATE, "that id is already imported");
    if (updateExisting&&!prototypeExists)
        return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the type to update is no longer imported");
    {
        std::lock_guard<std::mutex> lock(logic::registryMutex);
        if (logic::definitions.count(d->custom_id))
            return refuse(TC_COMPONENT_TYPES_ERR_DUPLICATE, "that id is already registered");
    }
    struct Rollback {
        TCGameModel& game; uint64_t id; bool removeOnFailure=false; bool committed=false;
        ~Rollback() { if(removeOnFailure&&!committed) game.removeCustomPrototype(id); }
    } rollback{game,d->custom_id,!replaceExisting};
    const auto bytes=component_definition::encode(shaped,logic::kMaxBridgePins,pinLane,
                                                   callbackV3!=nullptr,outputDependencies,
                                                   clockedTick);
    if (bytes.empty())
        return refuse(TC_COMPONENT_TYPES_ERR_ARGUMENT, "the definition could not be encoded");
    /* tc.component.runtime hands the same bytes back to its caller: the game
       reads a player component from <save>/schematics/foundry/<name>/, so a
       type registered while the game runs can be given that entry too. */
    if (encoded) *encoded = bytes;
    TCComponentImportResult imported{};
    const char* importWhy=nullptr;
    if(!importGeneratedDefinition(components,host,bytes,d->name,d->custom_id,replaceExisting,
                                  &imported,&importWhy))
        return refuse(TC_COMPONENT_TYPES_ERR_GAME,
                      importWhy?importWhy:"the definition could not be written");
    if (!imported.ok() || imported.custom_id!=d->custom_id)
        return refuse(TC_COMPONENT_TYPES_ERR_GAME,
                      replaceExisting?"the game refused to update the generated definition"
                                     :"the game refused to import the generated definition");
    TCPrototype prototype{};
    if (!game.getCustomPrototype(d->custom_id,prototype))
        return refuse(TC_COMPONENT_TYPES_ERR_GAME, "the imported prototype could not be read back");
    bool ok=game.setPrototypeGateCost(prototype,d->gate_cost) && game.setPrototypeDelay(prototype,d->delay);
    /* The game prints the prototype's description at the top of the bottom
       component drawer, which is where a Mod's property rows live, so a
       description is written into the prototype only when the definition asks
       for it (TC_COMPONENT_TYPE_SHOW_DESCRIPTION).  The text itself is never
       lost: the loader's own type catalogue keeps it for the Mods page. */
    const bool showDescription=d->description&&(declaredFlags&TC_COMPONENT_TYPE_SHOW_DESCRIPTION)!=0;
    if (showDescription)
        ok=ok && game.setPrototypeDescription(prototype,d->description);
    else if (d->description&&host&&host->log) {
        char hidden[192]{};
        std::snprintf(hidden,sizeof(hidden),
                      "Component V2: type 0x%llx keeps its description out of the game's "
                      "panel (set TC_COMPONENT_TYPE_SHOW_DESCRIPTION to show it)",
                      static_cast<unsigned long long>(d->custom_id));
        host->log(host->context,hidden);
    }
    if (showDescription&&host&&host->log) {
        char shown[160]{};
        std::snprintf(shown,sizeof(shown),
                      "Component V2: type 0x%llx shows its description in the game's panel "
                      "(TC_COMPONENT_TYPE_SHOW_DESCRIPTION)",
                      static_cast<unsigned long long>(d->custom_id));
        host->log(host->context,shown);
    }
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
    if(preciseDependencies)
        registration.outputDependencies.assign(outputDependencies,
                                                outputDependencies+d->output_count);
    registration.clockedTick=clockedTick;
    registration.groupedOutputs=groupedOutputs;
    if(inlineExpressions) {
        registration.inlinePrefixes.resize(d->output_count);
        registration.inlineExpressions.resize(d->output_count);
        for(uint32_t i=0;i<d->output_count;++i) {
            if(d->inline_prefixes&&d->inline_prefixes[i])
                registration.inlinePrefixes[i]=d->inline_prefixes[i];
            registration.inlineExpressions[i]=d->inline_expressions[i];
        }
    }
    int bridged=0;
    if(callbackV3) {
        logic::V3Registration v3;
        v3.custom_id=registration.custom_id;
        v3.callback=callbackV3;
        v3.user=registration.user;
        v3.stateWords=registration.stateWords;
        v3.owner_mod=registration.owner_mod;
        v3.lifecycle=registration.lifecycle;
        v3.configSchema=registration.configSchema;
        v3.defaultConfig=registration.defaultConfig;
        v3.migrationVersion=registration.migrationVersion;
        v3.migrateConfig=registration.migrateConfig;
        v3.migrationUser=registration.migrationUser;
        v3.outputDependencies=registration.outputDependencies;
        v3.clockedTick=registration.clockedTick;
        v3.groupedOutputs=registration.groupedOutputs;
        v3.inlinePrefixes=registration.inlinePrefixes;
        v3.inlineExpressions=registration.inlineExpressions;
        bridged=logic::addV3(v3);
    } else bridged=logic::addV2(registration);
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

/* V3 has the same definition metadata and optional tails as V2; only the logic
   callback type and generated output driver differ.  Copy fields explicitly so
   no ABI aliasing or function-pointer cast is hidden in the registration path. */
inline int registerNativeComponentV3(const TCHost* host,
                                     const TCComponentTypeDefinitionV3* d,
                                     std::vector<uint8_t>* encoded=nullptr,
                                     bool updateExisting=false,
                                     bool adoptFoundryEntry=false) {
    if(!d)return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    const size_t requiredSize=offsetof(TCComponentTypeDefinitionV3,lifecycle);
    if(d->size<requiredSize||d->version!=TC_COMPONENT_TYPES_VERSION_3||!d->callback)
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    TCComponentTypeDefinitionV2 v2{};
    v2.size=sizeof(v2);v2.version=TC_COMPONENT_TYPES_VERSION_2;
    v2.custom_id=d->custom_id;v2.type_id=d->type_id;v2.name=d->name;
    v2.description=d->description;v2.shape_svg=d->shape_svg;
    v2.inputs=d->inputs;v2.outputs=d->outputs;
    v2.input_count=d->input_count;v2.output_count=d->output_count;
    v2.state_words=d->state_words;v2.reserved=d->reserved;
    v2.gate_cost=d->gate_cost;v2.delay=d->delay;v2.callback=nullptr;v2.user=d->user;
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,lifecycle)+sizeof(d->lifecycle))
        v2.lifecycle=d->lifecycle;
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,default_config)+sizeof(d->default_config)) {
        v2.config_schema=d->config_schema;v2.config_size=d->config_size;
        v2.default_config=d->default_config;
    }
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,migration_user)+sizeof(d->migration_user)) {
        v2.config_migration_version=d->config_migration_version;
        v2.reserved2=d->reserved2;v2.migrate_config=d->migrate_config;
        v2.migration_user=d->migration_user;
    }
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,pin_lane)+sizeof(d->pin_lane)) {
        v2.pin_lane=d->pin_lane;v2.reserved3=d->reserved3;
    }
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,flags)+sizeof(d->flags)) {
        v2.flags=d->flags;v2.reserved4=d->reserved4;
    }
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,output_dependency_count)+
               sizeof(d->output_dependency_count)) {
        v2.output_dependencies=d->output_dependencies;
        v2.output_dependency_count=d->output_dependency_count;
        v2.reserved5=d->reserved5;
    }
    if(d->size>=offsetof(TCComponentTypeDefinitionV3,inline_expressions)+
               sizeof(d->inline_expressions)) {
        v2.inline_expression_version=d->inline_expression_version;
        v2.inline_expression_count=d->inline_expression_count;
        v2.inline_prefixes=d->inline_prefixes;
        v2.inline_expressions=d->inline_expressions;
    }
    return registerNativeComponentV2(host,&v2,d->callback,encoded,updateExisting,adoptFoundryEntry);
}
}
