#ifndef TC_SAVE_MODEL_H
#define TC_SAVE_MODEL_H

#include "tc_game_model.h"

namespace tc {

using TCEmutlsGetAddressFn = void* (*)(void* control);

struct TCSaveModel {
    const int64_t* save_count = nullptr;
    void* save_level_control = nullptr;
    void* save_schematic_control = nullptr;
    TCEmutlsGetAddressFn emutls_get_address = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        save_count = static_cast<const int64_t*>(
            host->resolve_symbol(host->context, "save_count__modelZsave_u11"));
        save_level_control = host->resolve_symbol(
            host->context,
            "__emutls_v.global_save_level_path__modelZmodel95types_u79");
        save_schematic_control = host->resolve_symbol(
            host->context,
            "__emutls_v.global_save_schematic_path__modelZmodel95types_u81");
        emutls_get_address = reinterpret_cast<TCEmutlsGetAddressFn>(
            host->resolve_symbol(host->context, "__emutls_get_address"));
        return valid();
    }

    bool valid() const {
        return save_count != nullptr && save_level_control != nullptr &&
               save_schematic_control != nullptr &&
               emutls_get_address != nullptr;
    }

    int64_t saveCount() const {
        return save_count ? *save_count : 0;
    }

    TCNimString levelPath() const {
        return tlsString(save_level_control);
    }

    TCNimString schematicPath() const {
        return tlsString(save_schematic_control);
    }

    const char* levelPathCStr() const {
        return nimStringCStr(levelPath());
    }

    const char* schematicPathCStr() const {
        return nimStringCStr(schematicPath());
    }

 private:
    TCNimString tlsString(void* control) const {
        TCNimString value{};
        if (!emutls_get_address || !control) return value;
        void* addr = emutls_get_address(control);
        if (addr) memcpy(&value, addr, sizeof(value));
        return value;
    }

    static const char* nimStringCStr(const TCNimString& value) {
        return value.data ? static_cast<const char*>(value.data) + 8 : nullptr;
    }
};

}  // namespace tc

#endif  // TC_SAVE_MODEL_H
