#ifndef TC_MOD_SETTINGS_H
#define TC_MOD_SETTINGS_H

#include "tc_mod_api.h"

#include <cstring>

namespace tc::mod_settings {

using Api = TCModSettingsApiV1;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_MOD_SETTINGS,
                            TC_MOD_SETTINGS_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_MOD_SETTINGS_API_VERSION_1 || !queried.context ||
        !queried.set_fields || !queried.clear_fields || !queried.get || !queried.reset)
        return false;
    *out = queried;
    return true;
}

inline int setFields(const Api& api, const TCModSettingFieldV1* fields,
                     uint32_t count, TCModSettingChangedV1 changed = nullptr,
                     void* user = nullptr) {
    return api.set_fields ? api.set_fields(api.context, fields, count, changed, user)
                          : TC_MOD_SETTINGS_ERR_UNAVAILABLE;
}

inline int get(const Api& api, const char* id, TCModSettingValueV1* out) {
    if (!out) return TC_MOD_SETTINGS_ERR_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    return api.get ? api.get(api.context, id, out, sizeof(*out))
                   : TC_MOD_SETTINGS_ERR_UNAVAILABLE;
}

inline int reset(const Api& api, const char* id) {
    return api.reset ? api.reset(api.context, id) : TC_MOD_SETTINGS_ERR_UNAVAILABLE;
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_MOD_SETTINGS_OK: return "ok";
        case TC_MOD_SETTINGS_ERR_UNAVAILABLE: return "this loader has no tc.mod.settings";
        case TC_MOD_SETTINGS_ERR_ARGUMENT: return "invalid field table or value";
        case TC_MOD_SETTINGS_ERR_DUPLICATE: return "duplicate setting id or storage key";
        case TC_MOD_SETTINGS_ERR_CAPACITY: return "too many settings";
        case TC_MOD_SETTINGS_ERR_STORAGE: return "the setting could not be persisted";
        case TC_MOD_SETTINGS_ERR_NOT_FOUND: return "the setting does not exist";
        case TC_MOD_SETTINGS_ERR_TYPE: return "unknown or mismatched setting type";
        case TC_MOD_SETTINGS_ERR_THREAD: return "settings must be used on the game thread";
        default: return "unknown status";
    }
}

}  // namespace tc::mod_settings

#endif  // TC_MOD_SETTINGS_H
