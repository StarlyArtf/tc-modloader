#include "../src/mod_storage.hpp"
#include "../sdk/tc_mod_storage.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {
struct Context {
    tc::mod_storage::Store* store = nullptr;
    std::string owner;
};

int readValue(void* raw, uint32_t scope, const char* key, void* out,
              uint32_t capacity, uint32_t* bytes) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->readValue(context.owner, scope, key, out, capacity, bytes);
}
int writeValue(void* raw, uint32_t scope, const char* key, const void* data,
               uint32_t size) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->writeValue(context.owner, scope, key, data, size);
}
int removeValue(void* raw, uint32_t scope, const char* key) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->removeValue(context.owner, scope, key);
}
int hasValue(void* raw, uint32_t scope, const char* key) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->hasValue(context.owner, scope, key);
}
int getVersion(void* raw, uint32_t scope, const char* key, uint32_t* version) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->getVersion(context.owner, scope, key, version);
}
int migrateValue(void* raw, uint32_t scope, const char* key, uint32_t target,
                 const void* defaults, uint32_t defaultSize,
                 TCModStorageMigrateV1 callback, void* user, uint32_t* resulting) {
    auto& context = *static_cast<Context*>(raw);
    return context.store->migrateValue(context.owner, scope, key, target,
                                       defaults, defaultSize, callback, user, resulting);
}
int appendMigration(void*, uint32_t from, uint32_t to, const void* oldData,
                    uint32_t oldSize, void* newData, uint32_t capacity,
                    uint32_t* newSize) {
    assert(from == 0 && to == 2);
    const std::string next = std::string(static_cast<const char*>(oldData), oldSize) + "-schema2";
    if (next.size() > capacity) return TC_MOD_STORAGE_MIGRATE_REJECT;
    std::memcpy(newData, next.data(), next.size());
    *newSize = static_cast<uint32_t>(next.size());
    return TC_MOD_STORAGE_MIGRATE_OK;
}
int rejectMigration(void*, uint32_t, uint32_t, const void*, uint32_t,
                    void*, uint32_t, uint32_t*) {
    return TC_MOD_STORAGE_MIGRATE_REJECT;
}

int query(void* raw, const char* id, uint32_t version, void* out,
          uint32_t outSize) {
    if (!id || !out) return TC_SERVICE_ERR_ARGUMENT;
    if (std::strcmp(id, TC_SERVICE_MOD_STORAGE) != 0)
        return TC_SERVICE_ERR_UNAVAILABLE;
    if (version == TC_MOD_STORAGE_API_VERSION_1) {
        if (outSize < sizeof(TCModStorageApiV1)) return TC_SERVICE_ERR_SIZE;
        *static_cast<TCModStorageApiV1*>(out) = TCModStorageApiV1{
            sizeof(TCModStorageApiV1), TC_MOD_STORAGE_API_VERSION_1, raw,
            &readValue, &writeValue, &removeValue, &hasValue};
        return TC_SERVICE_OK;
    }
    if (version == TC_MOD_STORAGE_API_VERSION_2) {
        if (outSize < sizeof(TCModStorageApiV2)) return TC_SERVICE_ERR_SIZE;
        *static_cast<TCModStorageApiV2*>(out) = TCModStorageApiV2{
            sizeof(TCModStorageApiV2), TC_MOD_STORAGE_API_VERSION_2, raw,
            &readValue, &writeValue, &removeValue, &hasValue, &getVersion, &migrateValue};
        return TC_SERVICE_OK;
    }
    return TC_SERVICE_ERR_VERSION;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::absolute("build/mod-storage-test").lexically_normal();
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path game = root / "game";
    const fs::path data = game / "tc-modloader-data";
    const fs::path profileA = root / "profiles" / "profile-a";
    fs::create_directories(data);
    fs::create_directories(profileA);

    tc::mod_storage::Store store(game, data, profileA);
    Context ownerA{&store, "example.alpha"};
    Context ownerB{&store, "example.beta"};

    TCHost host{};
    host.size = sizeof(host);
    host.context = &ownerA;
    host.query_service = &query;
    tc::mod_storage::Api api{};
    assert(tc::mod_storage::table(&host, &api));
    tc::mod_storage::ApiV2 apiV2{};
    assert(tc::mod_storage::tableV2(&host, &apiV2));

    assert(tc::mod_storage::has(api, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings") == 0);
    assert(tc::mod_storage::writeText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                     "settings", "global-v1") == TC_MOD_STORAGE_OK);
    assert(tc::mod_storage::writeText(api, TC_MOD_STORAGE_SCOPE_PROFILE,
                                     "progress", "profile-a") == TC_MOD_STORAGE_OK);

    std::string text;
    assert(tc::mod_storage::readText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &text) == TC_MOD_STORAGE_OK);
    assert(text == "global-v1");
    assert(tc::mod_storage::readText(api, TC_MOD_STORAGE_SCOPE_PROFILE,
                                    "progress", &text) == TC_MOD_STORAGE_OK);
    assert(text == "profile-a");

    uint32_t bytes = 0;
    char shortBuffer[2]{};
    assert(store.readValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL,
                           "settings", shortBuffer, sizeof(shortBuffer), &bytes) ==
           TC_MOD_STORAGE_ERR_SIZE);
    assert(bytes == 9);

    /* The same key belongs to a different namespace for another Mod. */
    assert(store.hasValue(ownerB.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings") == 0);
    assert(store.readValue(ownerB.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings",
                           nullptr, 0, &bytes) == TC_MOD_STORAGE_ERR_NOT_FOUND);

    /* Empty and replaced values remain present; remove is the only operation
       that turns one into a missing key. */
    assert(store.writeValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "empty",
                            nullptr, 0) == TC_MOD_STORAGE_OK);
    assert(store.hasValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "empty") == 1);
    assert(store.readValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "empty",
                           nullptr, 0, &bytes) == TC_MOD_STORAGE_OK && bytes == 0);
    assert(tc::mod_storage::writeText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                     "settings", "global-v2") == TC_MOD_STORAGE_OK);
    assert(tc::mod_storage::remove(api, TC_MOD_STORAGE_SCOPE_GLOBAL, "empty") ==
           TC_MOD_STORAGE_OK);
    assert(tc::mod_storage::remove(api, TC_MOD_STORAGE_SCOPE_GLOBAL, "empty") ==
           TC_MOD_STORAGE_ERR_NOT_FOUND);

    uint32_t schema = 99, resulting = 0;
    assert(tc::mod_storage::version(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &schema) == TC_MOD_STORAGE_OK && schema == 0);
    assert(tc::mod_storage::migrate(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", 2, nullptr, 0,
                                    &appendMigration, nullptr, &resulting) == TC_MOD_STORAGE_OK &&
           resulting == 2);
    assert(tc::mod_storage::readText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &text) == TC_MOD_STORAGE_OK &&
           text == "global-v2-schema2");
    /* Old V1 writers replace the payload but retain schema metadata. */
    assert(tc::mod_storage::writeText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                     "settings", "v1-writer") == TC_MOD_STORAGE_OK);
    assert(tc::mod_storage::version(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &schema) == TC_MOD_STORAGE_OK && schema == 2);
    /* Rejection and downgrade leave the prior version and payload untouched. */
    assert(tc::mod_storage::migrate(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", 3, nullptr, 0,
                                    &rejectMigration, nullptr, &resulting) ==
           TC_MOD_STORAGE_ERR_MIGRATION);
    assert(tc::mod_storage::readText(api, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &text) == TC_MOD_STORAGE_OK && text == "v1-writer");
    assert(tc::mod_storage::version(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &schema) == TC_MOD_STORAGE_OK && schema == 2);
    assert(tc::mod_storage::migrate(apiV2, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", 1, nullptr, 0, nullptr, nullptr,
                                    &resulting) == TC_MOD_STORAGE_ERR_VERSION);
    const char defaults[] = "fresh";
    assert(tc::mod_storage::migrate(apiV2, TC_MOD_STORAGE_SCOPE_PROFILE,
                                    "new-value", 4, defaults, 5, nullptr, nullptr,
                                    &resulting) == TC_MOD_STORAGE_OK && resulting == 4);
    assert(tc::mod_storage::readText(apiV2, TC_MOD_STORAGE_SCOPE_PROFILE,
                                    "new-value", &text) == TC_MOD_STORAGE_OK && text == "fresh");

    assert(store.writeValue(ownerA.owner, 99, "bad", "x", 1) ==
           TC_MOD_STORAGE_ERR_ARGUMENT);
    assert(store.writeValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL,
                            "../escape", "x", 1) == TC_MOD_STORAGE_ERR_ARGUMENT);
    std::vector<unsigned char> tooLarge(TC_MOD_STORAGE_MAX_VALUE_BYTES + 1u);
    assert(store.writeValue(ownerA.owner, TC_MOD_STORAGE_SCOPE_GLOBAL, "large",
                            tooLarge.data(), static_cast<uint32_t>(tooLarge.size())) ==
           TC_MOD_STORAGE_ERR_SIZE);

    /* Save profile duplication is a directory copy.  PROFILE values live under
       that directory and therefore follow it; GLOBAL values do not. */
    const fs::path profileB = root / "profiles" / "profile-b";
    fs::copy(profileA, profileB, fs::copy_options::recursive);
    tc::mod_storage::Store copied(game, data, profileB);
    Context copiedOwner{&copied, "example.alpha"};
    TCHost copiedHost = host;
    copiedHost.context = &copiedOwner;
    tc::mod_storage::Api copiedApi{};
    assert(tc::mod_storage::table(&copiedHost, &copiedApi));
    assert(tc::mod_storage::readText(copiedApi, TC_MOD_STORAGE_SCOPE_PROFILE,
                                    "progress", &text) == TC_MOD_STORAGE_OK);
    assert(text == "profile-a");
    assert(tc::mod_storage::readText(copiedApi, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                    "settings", &text) == TC_MOD_STORAGE_OK);
    assert(text == "v1-writer");

    fs::remove_all(root, error);
    std::cout << "PASS tc.mod.storage V1/V2 isolation, atomic migration rollback and profile copy\n";
    return 0;
}
