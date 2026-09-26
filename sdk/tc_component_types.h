#ifndef TC_COMPONENT_TYPES_H
#define TC_COMPONENT_TYPES_H

#include "tc_mod_api.h"
#include "tc_service_api.h"
#include "tc_logic_api.h"

#include <stdint.h>

/* tc.component.types V1 - registering a component with the V2 value model.

   Use this instead of `TCNativeComponent::registerWith` when the component has
   more than eight pins on a side, or when it wants a state size of its own:

   ```cpp
   #include "tc_component_types.h"

   static void logic(TCLogicIOV2* io) {
       if (io->phase == TC_LOGIC_RESET) return;
       uint64_t acc = 0;
       for (uint32_t i = 0; i < io->input_count; ++i) acc ^= io->inputs[i];
       io->outputs[0] = acc & 1u;
   }

   static const TCComponentPinV2 ins[] = {
       {"a", "Input A", 1, 0}, {"b", "Input B", 1, 0},   // ... up to sixteen
   };
   static const TCComponentPinV2 outs[] = {{"y", "Output", 1, 0}};

   tc::component_types::Api types{};
   if (tc::component_types::table(host, &types)) {
       TCComponentTypeDefinitionV2 definition{};
       definition.size = sizeof(definition);
       definition.version = TC_COMPONENT_TYPES_VERSION_2;
       definition.custom_id = 0x4d5949445f303031ULL;   // stable across saves
       definition.name = "Xor many";
       definition.inputs = ins;  definition.input_count = 8;
       definition.outputs = outs; definition.output_count = 1;
       definition.callback = &logic;
       tc::component_types::registerDefinition(types, &definition);
   }
   ```

   Every pin is one uint64_t in the callback.  `bits` may be 1..64, and the sum
   of the input widths must stay within the host's budget (128 bits today: the
   generated call carries two 64-bit payload words, and a wider call shape does
   not survive the game's JIT - measured).  A direction may be empty (a pure
   source or a pure sink), and both may be empty: that is a decorative object
   which only owns a place on the board plus its per-instance configuration
   (see docs/research/text-component.md).

   A definition may also append a host-owned configuration blob with
   `config_schema`, `config_size` and `default_config`.  The host copies the
   default during registration and gives each instance its own copy; callbacks
   read it through the appended TCLogicIOV2 config fields.  Use
   tc_component_storage.h to replace an instance's complete configuration, and
   set `config_migration_version`/`migrate_config` to convert a configuration an
   earlier release of the same component saved under an older schema.  Without a
   migration, a record the loader cannot use is left untouched and the instance
   runs on its registered default (see docs/sdk/services.md). */

namespace tc {
namespace component_types {

using Api = TCComponentTypesApiV1;
using Pin = TCComponentPinV2;
using Definition = TCComponentTypeDefinitionV2;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_TYPES,
                            TC_COMPONENT_TYPES_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_TYPES_API_VERSION_1 || !queried.context ||
        !queried.register_definition)
        return false;
    *out = queried;
    return true;
}

/* Registers one definition.  Call during tc_mod_load, like the V1 entry. */
inline int registerDefinition(const Api& api, const Definition* definition) {
    if (!api.register_definition) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    return api.register_definition(api.context, definition);
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_COMPONENT_TYPES_OK: return "ok";
        case TC_COMPONENT_TYPES_ERR_UNAVAILABLE: return "this loader has no tc.component.types";
        case TC_COMPONENT_TYPES_ERR_ARGUMENT: return "the definition is malformed";
        case TC_COMPONENT_TYPES_ERR_DUPLICATE: return "that custom_id is already registered";
        case TC_COMPONENT_TYPES_ERR_UNSUPPORTED: return "the shape cannot be bridged";
        case TC_COMPONENT_TYPES_ERR_GAME: return "the game refused the definition";
        case TC_COMPONENT_TYPES_ERR_BUDGET: return "the pin widths exceed the host's budget";
        default: return "unknown status";
    }
}

}  // namespace component_types
}  // namespace tc

#endif  // TC_COMPONENT_TYPES_H
