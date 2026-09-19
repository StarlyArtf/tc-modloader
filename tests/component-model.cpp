#include "../sdk/tc_component_model.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <iostream>

static uint8_t errorFlag;
static int allocated, freed, imported, updated, destroyed;
static bool reject, failAfterCall;
static const uint64_t id = 0xf123456789ab007bULL;
static std::string read(const tc::TCNimString* s) {
    return std::string(static_cast<const char*>(s->data) + 8, s->length);
}
static void allocate(void* out, int64_t size) {
    auto* s = static_cast<tc::TCNimString*>(out);
    s->length = 0; // Match real rawNewString, not the former inaccurate mock.
    s->data = std::calloc(1, size + 9);
    ++allocated;
}
static void release(void* data) { ++freed; std::free(data); }
static void destroy(void*) { ++destroyed; }
static void* tls(void*) { return &errorFlag; }
static void* import(tc::TCComponentImportRawResult* out, const tc::TCNimString* name,
    const tc::TCNimString* data, const tc::TCNimString* dir) {
    ++imported;
    assert(read(name) == "Test");
    assert(read(data) == std::string("a\0b", 3));
    assert(read(dir) == "C:/fixture/");
    out->accepted = !reject;
    out->custom_id = id;
    if (failAfterCall) errorFlag = 1;
    return out;
}
static void* update(tc::TCComponentImportRawResult* out,
    const tc::TCNimString* dir, const tc::TCNimString* name) {
    ++updated;
    assert(read(name) == "Test" && read(dir) == "C:/fixture/");
    out->accepted = 1; out->custom_id = id; return out;
}
static void* resolve(void*, const char* name) {
    std::string s(name);
    if (s == "add_custom_prototype__modelZboardZcustom95prototype_u2718") return (void*)&import;
    if (s == "update_custom_prototype__modelZboardZcustom95prototype_u2734") return (void*)&update;
    if (s == "rawNewString") return (void*)&allocate;
    if (s == "deallocShared") return (void*)&release;
    if (s == "eqdestroy___modelZboardZprototype95list_u3259") return (void*)&destroy;
    if (s == "__emutls_get_address") return (void*)&tls;
    if (s == "__emutls_v.nimInErrorMode__system_u4319") return &errorFlag;
    return nullptr;
}
int main() {
    using S = tc::TCComponentStatus;
    tc::TCComponentModel m;
    assert(m.readiness() == S::Unavailable);
    TCHost h{}; h.resolve_symbol = resolve;
    assert(m.load(&h));
    auto r = m.importCircuit("Test", "a\0b", 3, "C:/fixture/");
    assert(r.ok() && r.custom_id == id && allocated == freed);
    assert(m.updateFromDirectory("C:/fixture/", "Test").ok());
    assert(updated == 1 && allocated == freed);
    auto count = imported;
    assert(m.importCircuit("Test", "a", 1, "C:/fixture").status == S::InvalidArgument);
    assert(m.importCircuit("Test", nullptr, 1, "C:/fixture/").status == S::InvalidArgument);
    assert(m.importCircuit("", "a", 1, "C:/fixture/").status == S::InvalidArgument);
    assert(m.importCircuit("Test", "a", 0, "C:/fixture/").status == S::InvalidArgument);
    assert(imported == count);
    std::thread worker([&] {
        assert(m.importCircuit("Test", "a\0b", 3, "C:/fixture/").status == S::WrongThread);
    }); worker.join();
    assert(imported == count);
    errorFlag = 1;
    assert(m.importCircuit("Test", "a\0b", 3, "C:/fixture/").status == S::NimError);
    assert(errorFlag == 1 && imported == count);
    errorFlag = 0; reject = true;
    assert(m.importCircuit("Test", "a\0b", 3, "C:/fixture/").status == S::Rejected);
    reject = false; failAfterCall = true;
    assert(m.importCircuit("Test", "a\0b", 3, "C:/fixture/").status == S::NimError);
    assert(errorFlag == 1 && allocated == freed);
    tc::TCPrototype p{};
    assert(m.releasePrototype(p) == S::NimError && destroyed == 0);
    errorFlag = 0;
    assert(m.releasePrototype(p) == S::Ok && destroyed == 1);
    assert(!m.load(nullptr) && m.readiness() == S::Unavailable);
    std::cout << "PASS component import ABI, binary strings, cleanup, rejection, thread and error guards\n";
}
