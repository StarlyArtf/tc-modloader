#ifndef TC_MOD_STORAGE_H
#define TC_MOD_STORAGE_H

#include "tc_mod_api.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tc::mod_storage {

using Api = TCModStorageApiV1;
using ApiV2 = TCModStorageApiV2;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_MOD_STORAGE,
                            TC_MOD_STORAGE_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_MOD_STORAGE_API_VERSION_1 || !queried.context ||
        !queried.read || !queried.write || !queried.remove || !queried.has)
        return false;
    *out = queried;
    return true;
}

inline bool tableV2(const TCHost* host, ApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_MOD_STORAGE,
                            TC_MOD_STORAGE_API_VERSION_2, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_MOD_STORAGE_API_VERSION_2 || !queried.context ||
        !queried.read || !queried.write || !queried.remove || !queried.has ||
        !queried.get_version || !queried.migrate)
        return false;
    *out = queried;
    return true;
}

template<class Table>
inline int read(const Table& api, uint32_t scope, const char* key,
                std::vector<std::uint8_t>* out) {
    if (!out || !api.read) return TC_MOD_STORAGE_ERR_ARGUMENT;
    uint32_t bytes = 0;
    int status = api.read(api.context, scope, key, nullptr, 0, &bytes);
    if (status != TC_MOD_STORAGE_OK) return status;
    std::vector<std::uint8_t> value(bytes);
    if (bytes) {
        uint32_t written = 0;
        status = api.read(api.context, scope, key, value.data(), bytes, &written);
        if (status != TC_MOD_STORAGE_OK) return status;
        if (written != bytes) return TC_MOD_STORAGE_ERR_IO;
    }
    *out = std::move(value);
    return TC_MOD_STORAGE_OK;
}

template<class Table>
inline int readText(const Table& api, uint32_t scope, const char* key,
                    std::string* out) {
    if (!out) return TC_MOD_STORAGE_ERR_ARGUMENT;
    std::vector<std::uint8_t> bytes;
    const int status = read(api, scope, key, &bytes);
    if (status != TC_MOD_STORAGE_OK) return status;
    if (bytes.empty()) out->clear();
    else out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return TC_MOD_STORAGE_OK;
}

template<class Table>
inline int write(const Table& api, uint32_t scope, const char* key,
                 const void* data, uint32_t size) {
    return api.write ? api.write(api.context, scope, key, data, size)
                     : TC_MOD_STORAGE_ERR_UNAVAILABLE;
}

template<class Table>
inline int writeText(const Table& api, uint32_t scope, const char* key,
                     const std::string& value) {
    if (value.size() > UINT32_MAX) return TC_MOD_STORAGE_ERR_SIZE;
    return write(api, scope, key, value.data(),
                 static_cast<uint32_t>(value.size()));
}

template<class Table>
inline int remove(const Table& api, uint32_t scope, const char* key) {
    return api.remove ? api.remove(api.context, scope, key)
                      : TC_MOD_STORAGE_ERR_UNAVAILABLE;
}

template<class Table>
inline int has(const Table& api, uint32_t scope, const char* key) {
    return api.has ? api.has(api.context, scope, key)
                   : TC_MOD_STORAGE_ERR_UNAVAILABLE;
}

inline int version(const ApiV2& api, uint32_t scope, const char* key,
                   uint32_t* schemaVersion) {
    return api.get_version
        ? api.get_version(api.context, scope, key, schemaVersion)
        : TC_MOD_STORAGE_ERR_UNAVAILABLE;
}

inline int migrate(const ApiV2& api, uint32_t scope, const char* key,
                   uint32_t targetVersion, const void* defaultData,
                   uint32_t defaultSize, TCModStorageMigrateV1 callback,
                   void* user, uint32_t* resultingVersion) {
    return api.migrate
        ? api.migrate(api.context, scope, key, targetVersion, defaultData,
                      defaultSize, callback, user, resultingVersion)
        : TC_MOD_STORAGE_ERR_UNAVAILABLE;
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_MOD_STORAGE_OK: return "ok";
        case TC_MOD_STORAGE_ERR_UNAVAILABLE: return "this loader has no tc.mod.storage";
        case TC_MOD_STORAGE_ERR_ARGUMENT: return "invalid scope, key, pointer or size";
        case TC_MOD_STORAGE_ERR_NOT_FOUND: return "the value does not exist";
        case TC_MOD_STORAGE_ERR_SIZE: return "the buffer or value size is invalid";
        case TC_MOD_STORAGE_ERR_IO: return "the host could not read or atomically replace the value";
        case TC_MOD_STORAGE_ERR_QUOTA: return "the Mod's storage quota is exhausted";
        case TC_MOD_STORAGE_ERR_VERSION: return "the stored schema is newer than the requested schema";
        case TC_MOD_STORAGE_ERR_MIGRATION: return "the migration was rejected or raced with another write";
        default: return "unknown status";
    }
}

}  // namespace tc::mod_storage

#endif  // TC_MOD_STORAGE_H
