#ifndef TC_COMPONENT_MODEL_H
#define TC_COMPONENT_MODEL_H

#include "tc_game_model.h"
#include <thread>

namespace tc {

// Windows x64 machine-level ABI: explicit hidden result buffer FIRST.
// This is not a function returning this struct by ordinary C++ value.
struct TCComponentImportRawResult {
    uint8_t accepted;
    uint8_t padding[7];
    uint64_t custom_id;
};
static_assert(sizeof(TCComponentImportRawResult) == 16, "Import result ABI");
static_assert(offsetof(TCComponentImportRawResult, custom_id) == 8, "Import ID ABI");
using TCImportCircuitFn = void* (*)(TCComponentImportRawResult*,
    const TCNimString* name, const TCNimString* bytes, const TCNimString* directory);
using TCUpdateCircuitFn = void* (*)(TCComponentImportRawResult*,
    const TCNimString* directory, const TCNimString* name);

enum class TCComponentStatus {
    Ok, Unavailable, WrongThread, InvalidArgument, NimError, Rejected
};
struct TCComponentImportResult {
    TCComponentStatus status = TCComponentStatus::Unavailable;
    uint64_t custom_id = 0;
    bool ok() const { return status == TCComponentStatus::Ok; }
};

// Experimental circuit-backed component import for the pinned build.
// Bind on the game main/render thread; call there while simulation is stopped.
// Import can replace an existing ID from the serialized circuit. Failure is
// NOT transactional. Never clear Nim's error flag to "recover" a failed call.
// This does not add a new native kind or register a per-cycle callback.
class TCComponentModel {
 public:
    bool load(const TCHost* host) {
        *this = TCComponentModel{};
        if (!host || !host->resolve_symbol) return false;
        auto resolve = [&](const char* s) { return host->resolve_symbol(host->context, s); };
        import_ = reinterpret_cast<TCImportCircuitFn>(resolve(
            "add_custom_prototype__modelZboardZcustom95prototype_u2718"));
        update_ = reinterpret_cast<TCUpdateCircuitFn>(resolve(
            "update_custom_prototype__modelZboardZcustom95prototype_u2734"));
        allocate_ = reinterpret_cast<TCRawNewStringFn>(resolve("rawNewString"));
        free_ = reinterpret_cast<void (*)(void*)>(resolve("deallocShared"));
        destroy_ = reinterpret_cast<void (*)(void*)>(resolve(
            "eqdestroy___modelZboardZprototype95list_u3259"));
        tls_ = resolve("__emutls_v.nimInErrorMode__system_u4319");
        tls_address_ = reinterpret_cast<void* (*)(void*)>(resolve("__emutls_get_address"));
        owner_ = std::this_thread::get_id();
        return valid();
    }

    bool valid() const {
        return import_ && update_ && allocate_ && free_ && destroy_ && tls_ && tls_address_;
    }

    TCComponentStatus readiness() const {
        if (!valid()) return TCComponentStatus::Unavailable;
        if (owner_ != std::this_thread::get_id()) return TCComponentStatus::WrongThread;
        auto* error = static_cast<const uint8_t*>(tls_address_(tls_));
        return !error ? TCComponentStatus::Unavailable :
            (*error ? TCComponentStatus::NimError : TCComponentStatus::Ok);
    }

    // The bytes are serialized circuit.data, NOT JSON or a Prototype memory dump.
    // directory is UTF-8 and must end in / or \\ (game concatenates filenames).
    TCComponentImportResult importCircuit(const char* name, const void* bytes,
        size_t length, const char* directory) const {
        auto state = readiness();
        if (state != TCComponentStatus::Ok) return {state, 0};
        if (!name || !*name || !bytes || !length || length > INT64_MAX - 9 ||
            !validDirectory(directory)) return {TCComponentStatus::InvalidArgument, 0};
        String n(*this), b(*this), d(*this);
        if (!n.set(name, strlen(name)) || !b.set(bytes, length) ||
            !d.set(directory, strlen(directory))) return {allocationStatus(), 0};
        TCComponentImportRawResult raw{};
        import_(&raw, &n.value, &b.value, &d.value);
        return result(raw);
    }

    // Read directory + "circuit.data" through the game's normal update path.
    TCComponentImportResult updateFromDirectory(const char* directory, const char* name) const {
        auto state = readiness();
        if (state != TCComponentStatus::Ok) return {state, 0};
        if (!validDirectory(directory) || !name || !*name)
            return {TCComponentStatus::InvalidArgument, 0};
        String d(*this), n(*this);
        if (!d.set(directory, strlen(directory)) || !n.set(name, strlen(name)))
            return {allocationStatus(), 0};
        TCComponentImportRawResult raw{};
        update_(&raw, &d.value, &n.value);
        return result(raw);
    }

    // Only for an OWNED snapshot returned by a game prototype getter.
    // Never use on a table entry, shallow byte copy, or builder with borrowed pins.
    TCComponentStatus releasePrototype(TCPrototype& owned) const {
        auto state = readiness();
        if (state != TCComponentStatus::Ok) return state;
        destroy_(&owned);
        memset(&owned, 0, sizeof(owned));
        return readiness();
    }

 private:
    TCImportCircuitFn import_ = nullptr;
    TCUpdateCircuitFn update_ = nullptr;
    TCRawNewStringFn allocate_ = nullptr;
    void (*free_)(void*) = nullptr;
    void (*destroy_)(void*) = nullptr;
    void* tls_ = nullptr;
    void* (*tls_address_)(void*) = nullptr;
    std::thread::id owner_{};
    struct String {
        const TCComponentModel& model;
        TCNimString value{};
        explicit String(const TCComponentModel& m) : model(m) {}
        String(const String&) = delete;
        String& operator=(const String&) = delete;
        ~String() { if (value.data) model.free_(value.data); }
        bool set(const void* data, size_t size) {
            if (!data || size > INT64_MAX - 9) return false;
            if (!size) return true;
            model.allocate_(&value, static_cast<int64_t>(size));
            if (!value.data || model.readiness() != TCComponentStatus::Ok) return false;
            value.length = size;
            memcpy(static_cast<unsigned char*>(value.data) + 8, data, size);
            static_cast<unsigned char*>(value.data)[size + 8] = 0;
            return true;
        }
    };
    static bool validDirectory(const char* directory) {
        if (!directory || !*directory) return false;
        size_t n = strlen(directory);
        return directory[n - 1] == '/' || directory[n - 1] == '\\';
    }
    TCComponentStatus allocationStatus() const {
        auto state = readiness();
        return state == TCComponentStatus::Ok ? TCComponentStatus::Rejected : state;
    }
    TCComponentImportResult result(const TCComponentImportRawResult& raw) const {
        auto state = readiness();
        if (state != TCComponentStatus::Ok) return {state, raw.custom_id};
        return {raw.accepted && raw.custom_id ? TCComponentStatus::Ok :
            TCComponentStatus::Rejected, raw.custom_id};
    }
};
} // namespace tc
#endif
