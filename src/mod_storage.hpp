#pragma once

#include "core.hpp"
#include "../sdk/tc_service_api.h"

#include <mutex>
#include <limits>

namespace tc::mod_storage {

/* Loader-owned storage for native Mods.  The public service never receives a
   path: ownership comes from the Loaded object used as the service context,
   and a key is deliberately a single, lowercase file-name component. */
class Store {
    fs::path gameRoot_;
    fs::path dataRoot_;
    fs::path saveRoot_;
    mutable std::mutex mutex_;

    struct Value {
        uint32_t schema = 0;
        std::string payload;
    };

    static constexpr unsigned char kEnvelopeMagic[8] = {
        'T', 'C', 'M', 'S', 'V', '2', '\r', '\n'};
    static constexpr uint64_t kEnvelopeTail = 0x32564752534d4354ull;

    static uint64_t checksum(const std::string& value) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template<class T>
    static void appendLittle(std::string& out, T value) {
        for (size_t i = 0; i < sizeof(T); ++i)
            out.push_back(static_cast<char>((value >> (i * 8)) & 0xffu));
    }

    template<class T>
    static T readLittle(const char* at) {
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(static_cast<unsigned char>(at[i])) << (i * 8);
        return value;
    }

    static std::string envelope(uint32_t schema, const std::string& payload) {
        std::string out;
        out.reserve(32u + payload.size());
        out.append(reinterpret_cast<const char*>(kEnvelopeMagic), sizeof(kEnvelopeMagic));
        appendLittle<uint32_t>(out, schema);
        appendLittle<uint32_t>(out, static_cast<uint32_t>(payload.size()));
        appendLittle<uint64_t>(out, checksum(payload));
        out += payload;
        appendLittle<uint64_t>(out, kEnvelopeTail);
        return out;
    }

    static Value decode(const std::string& stored) {
        if (stored.size() < sizeof(kEnvelopeMagic) ||
            std::memcmp(stored.data(), kEnvelopeMagic, sizeof(kEnvelopeMagic)) != 0)
            return Value{0u, stored};
        if (stored.size() < 32u) throw std::runtime_error("truncated storage envelope");
        const uint32_t schema = readLittle<uint32_t>(stored.data() + 8);
        const uint32_t bytes = readLittle<uint32_t>(stored.data() + 12);
        const uint64_t expected = readLittle<uint64_t>(stored.data() + 16);
        if (bytes > TC_MOD_STORAGE_MAX_VALUE_BYTES ||
            stored.size() != static_cast<size_t>(bytes) + 32u ||
            readLittle<uint64_t>(stored.data() + 24u + bytes) != kEnvelopeTail)
            throw std::runtime_error("invalid storage envelope");
        std::string payload(stored.data() + 24u, stored.data() + 24u + bytes);
        if (checksum(payload) != expected)
            throw std::runtime_error("storage envelope checksum mismatch");
        return Value{schema, std::move(payload)};
    }

    static bool validKey(const char* key) {
        if (!key) return false;
        const size_t length = std::strlen(key);
        if (!length || length > TC_MOD_STORAGE_MAX_KEY_BYTES) return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(key[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-'))
                return false;
        }
        const std::string file = std::string(key) + ".bin";
        return safe(file);
    }

    fs::path scopeRoot(uint32_t scope, const std::string& owner,
                       fs::path* anchor) const {
        if (!valid_id(owner)) throw std::invalid_argument("invalid owner");
        if (scope == TC_MOD_STORAGE_SCOPE_GLOBAL) {
            if (anchor) *anchor = gameRoot_;
            return dataRoot_ / L"plugin-data" / fs::u8path(owner) / L"storage";
        }
        if (scope == TC_MOD_STORAGE_SCOPE_PROFILE) {
            if (anchor) *anchor = saveRoot_;
            return saveRoot_ / L"tc-mod-data" / fs::u8path(owner);
        }
        throw std::invalid_argument("invalid scope");
    }

    fs::path valuePath(uint32_t scope, const std::string& owner,
                       const char* key, fs::path* anchor,
                       fs::path* directory = nullptr) const {
        if (!validKey(key)) throw std::invalid_argument("invalid key");
        auto base = scopeRoot(scope, owner, anchor);
        if (directory) *directory = base;
        return base / fs::u8path(std::string(key) + ".bin");
    }

    static std::pair<uint32_t, uint64_t> usage(const fs::path& anchor,
                                                const fs::path& directory) {
        std::error_code error;
        const bool present = fs::exists(directory, error);
        if (error) throw std::runtime_error("storage directory is unavailable");
        if (!present) return {0, 0};
        if (!fs::is_directory(directory, error) || error)
            throw std::runtime_error("storage directory is unavailable");
        uint32_t count = 0;
        uint64_t bytes = 0;
        for (const auto& item : fs::directory_iterator(directory)) {
            no_links(anchor, item.path());
            if (!item.is_regular_file()) continue;
            const auto value = decode(read(item.path()));
            const auto size = value.payload.size();
            if (++count > TC_MOD_STORAGE_MAX_KEYS ||
                size > TC_MOD_STORAGE_MAX_VALUE_BYTES ||
                bytes > TC_MOD_STORAGE_MAX_SCOPE_BYTES - size)
                throw std::runtime_error("storage quota is already exceeded");
            bytes += size;
        }
        return {count, bytes};
    }

    Value readPath(const fs::path& path) const {
        std::error_code error;
        if (!fs::exists(path, error)) {
            if (error) throw std::runtime_error("storage value is unavailable");
            throw fs::filesystem_error("missing storage value", path,
                                       std::make_error_code(std::errc::no_such_file_or_directory));
        }
        if (!fs::is_regular_file(path, error) || error)
            throw std::runtime_error("storage value is unavailable");
        return decode(read(path));
    }

    int writeVersionedUnlocked(const std::string& owner, uint32_t scope,
                               const char* key, uint32_t schema,
                               const std::string& payload) {
        fs::path anchor, directory;
        const auto path = valuePath(scope, owner, key, &anchor, &directory);
        no_links(anchor, directory);
        no_links(anchor, path);
        fs::create_directories(directory);
        no_links(anchor, directory);
        no_links(anchor, path);

        auto [count, total] = usage(anchor, directory);
        std::error_code error;
        const bool replacing = fs::is_regular_file(path, error) && !error;
        uint64_t previous = 0;
        if (replacing) previous = readPath(path).payload.size();
        if (!replacing && count >= TC_MOD_STORAGE_MAX_KEYS)
            return TC_MOD_STORAGE_ERR_QUOTA;
        if (previous > total || total - previous + payload.size() >
                                    TC_MOD_STORAGE_MAX_SCOPE_BYTES)
            return TC_MOD_STORAGE_ERR_QUOTA;
        atomic(path, envelope(schema, payload));
        return TC_MOD_STORAGE_OK;
    }

public:
    Store(fs::path gameRoot, fs::path dataRoot, fs::path saveRoot)
        : gameRoot_(fs::absolute(std::move(gameRoot)).lexically_normal()),
          dataRoot_(fs::absolute(std::move(dataRoot)).lexically_normal()),
          saveRoot_(fs::absolute(std::move(saveRoot)).lexically_normal()) {}

    int readValue(const std::string& owner, uint32_t scope, const char* key,
                  void* out, uint32_t capacity, uint32_t* bytes) const {
        if (!bytes || (!out && capacity)) return TC_MOD_STORAGE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            no_links(anchor, path);
            std::error_code error;
            const bool present = fs::exists(path, error);
            if (error) return TC_MOD_STORAGE_ERR_IO;
            if (!present) {
                *bytes = 0;
                return TC_MOD_STORAGE_ERR_NOT_FOUND;
            }
            const auto value = readPath(path);
            const auto size = value.payload.size();
            *bytes = static_cast<uint32_t>(size);
            if (!out && capacity == 0) return TC_MOD_STORAGE_OK;
            if (capacity < size) return TC_MOD_STORAGE_ERR_SIZE;
            if (!value.payload.empty())
                std::memcpy(out, value.payload.data(), value.payload.size());
            return TC_MOD_STORAGE_OK;
        } catch (const std::invalid_argument&) {
            if (bytes) *bytes = 0;
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            if (bytes) *bytes = 0;
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int writeValue(const std::string& owner, uint32_t scope, const char* key,
                   const void* data, uint32_t size) {
        if ((!data && size) || size > TC_MOD_STORAGE_MAX_VALUE_BYTES)
            return size > TC_MOD_STORAGE_MAX_VALUE_BYTES
                       ? TC_MOD_STORAGE_ERR_SIZE
                       : TC_MOD_STORAGE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            const char* begin = static_cast<const char*>(data);
            std::string payload = size ? std::string(begin, begin + size) : std::string();
            uint32_t schema = 0;
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            std::error_code error;
            if (fs::exists(path, error) && !error) schema = readPath(path).schema;
            else if (error) return TC_MOD_STORAGE_ERR_IO;
            return writeVersionedUnlocked(owner, scope, key, schema, payload);
        } catch (const std::invalid_argument&) {
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int removeValue(const std::string& owner, uint32_t scope, const char* key) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            no_links(anchor, path);
            std::error_code error;
            const bool present = fs::exists(path, error);
            if (error) return TC_MOD_STORAGE_ERR_IO;
            if (!present) return TC_MOD_STORAGE_ERR_NOT_FOUND;
            if (!fs::is_regular_file(path, error) || error)
                return TC_MOD_STORAGE_ERR_IO;
            if (!fs::remove(path, error) || error) return TC_MOD_STORAGE_ERR_IO;
            return TC_MOD_STORAGE_OK;
        } catch (const std::invalid_argument&) {
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int hasValue(const std::string& owner, uint32_t scope, const char* key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            no_links(anchor, path);
            std::error_code error;
            const bool present = fs::exists(path, error);
            if (error) return TC_MOD_STORAGE_ERR_IO;
            if (!present) return 0;
            return fs::is_regular_file(path, error) && !error
                       ? 1
                       : TC_MOD_STORAGE_ERR_IO;
        } catch (const std::invalid_argument&) {
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int getVersion(const std::string& owner, uint32_t scope, const char* key,
                   uint32_t* schema) const {
        if (!schema) return TC_MOD_STORAGE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            no_links(anchor, path);
            std::error_code error;
            if (!fs::exists(path, error)) {
                *schema = 0;
                return error ? TC_MOD_STORAGE_ERR_IO : TC_MOD_STORAGE_ERR_NOT_FOUND;
            }
            *schema = readPath(path).schema;
            return TC_MOD_STORAGE_OK;
        } catch (const std::invalid_argument&) {
            *schema = 0;
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            *schema = 0;
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int writeVersioned(const std::string& owner, uint32_t scope, const char* key,
                       uint32_t schema, const void* data, uint32_t size) {
        if ((!data && size) || size > TC_MOD_STORAGE_MAX_VALUE_BYTES)
            return size > TC_MOD_STORAGE_MAX_VALUE_BYTES ? TC_MOD_STORAGE_ERR_SIZE
                                                         : TC_MOD_STORAGE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            const char* begin = static_cast<const char*>(data);
            return writeVersionedUnlocked(owner, scope, key, schema,
                size ? std::string(begin, begin + size) : std::string());
        } catch (const std::invalid_argument&) {
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            return TC_MOD_STORAGE_ERR_IO;
        }
    }

    int migrateValue(const std::string& owner, uint32_t scope, const char* key,
                     uint32_t target, const void* defaultData, uint32_t defaultSize,
                     TCModStorageMigrateV1 callback, void* user, uint32_t* resulting) {
        if (!target || !resulting || (!defaultData && defaultSize) ||
            defaultSize > TC_MOD_STORAGE_MAX_VALUE_BYTES)
            return defaultSize > TC_MOD_STORAGE_MAX_VALUE_BYTES ? TC_MOD_STORAGE_ERR_SIZE
                                                                : TC_MOD_STORAGE_ERR_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_);
        try {
            fs::path anchor;
            const auto path = valuePath(scope, owner, key, &anchor);
            no_links(anchor, path);
            std::error_code error;
            const bool present = fs::exists(path, error);
            if (error) return TC_MOD_STORAGE_ERR_IO;
            if (!present) {
                const char* begin = static_cast<const char*>(defaultData);
                const std::string payload = defaultSize
                    ? std::string(begin, begin + defaultSize) : std::string();
                const int status = writeVersionedUnlocked(owner, scope, key, target, payload);
                if (status == TC_MOD_STORAGE_OK) *resulting = target;
                return status;
            }
            const Value before = readPath(path);
            *resulting = before.schema;
            if (before.schema == target) return TC_MOD_STORAGE_OK;
            if (before.schema > target) return TC_MOD_STORAGE_ERR_VERSION;
            if (!callback) return TC_MOD_STORAGE_ERR_MIGRATION;

            std::vector<unsigned char> next(TC_MOD_STORAGE_MAX_VALUE_BYTES);
            uint32_t nextSize = 0;
            lock.unlock();
            int callbackStatus = TC_MOD_STORAGE_MIGRATE_REJECT;
            try {
                callbackStatus = callback(user, before.schema, target,
                    before.payload.empty() ? nullptr : before.payload.data(),
                    static_cast<uint32_t>(before.payload.size()), next.data(),
                    static_cast<uint32_t>(next.size()), &nextSize);
            } catch (...) {
                callbackStatus = TC_MOD_STORAGE_MIGRATE_REJECT;
            }
            if (callbackStatus != TC_MOD_STORAGE_MIGRATE_OK)
                return TC_MOD_STORAGE_ERR_MIGRATION;
            if (nextSize > TC_MOD_STORAGE_MAX_VALUE_BYTES)
                return TC_MOD_STORAGE_ERR_SIZE;
            lock.lock();
            const Value current = readPath(path);
            if (current.schema != before.schema || current.payload != before.payload)
                return TC_MOD_STORAGE_ERR_MIGRATION;
            const std::string payload(reinterpret_cast<const char*>(next.data()), nextSize);
            const int status = writeVersionedUnlocked(owner, scope, key, target, payload);
            if (status == TC_MOD_STORAGE_OK) *resulting = target;
            return status;
        } catch (const std::invalid_argument&) {
            return TC_MOD_STORAGE_ERR_ARGUMENT;
        } catch (...) {
            return TC_MOD_STORAGE_ERR_IO;
        }
    }
};

}  // namespace tc::mod_storage
