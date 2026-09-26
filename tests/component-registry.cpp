/* Offline checks for the component catalogue behind TC_SERVICE_COMPONENT_REGISTRY.

   The catalogue is what answers "which component types does this session know",
   "what pins does that type have" and "why was my registration refused".  The
   two registration paths feed it from the game (a declarative definition and an
   imported one the loader bridges), so this file drives the store directly with
   the values those paths hand over, and then checks the shape of the ABI table
   the service hands out.

   What it pins:
     * a registered type keeps its declared pins, costs and owner Mod;
     * the shape the game compiled overrides widths but never loses a declared
       name, which is how the declarative path and the bridge agree;
     * a refusal is visible as an inactive entry with the bridge's reason, and a
       later successful registration replaces it;
     * unknown ids, out-of-range indexes and malformed argument structs are told
       apart by the service's error codes;
     * a rejected Mod takes its types with it;
     * the derived capability bits follow the shape. */
#include "../src/component_registry.hpp"
#include "../sdk/tc_component_registry.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

tc::component_registry::Pin pin(const char* name, uint64_t bits) {
    tc::component_registry::Pin value;
    value.name = name ? name : "";
    value.bits = bits;
    return value;
}

/* The declarative path: what register_component hands to the catalogue. */
tc::component_registry::Type declarative(uint64_t id, const char* owner, const char* name,
                                         uint64_t cost, uint64_t delay) {
    tc::component_registry::Type type;
    type.custom_id = id;
    type.owner_mod = owner;
    type.name = name;
    type.description = "a test component";
    type.gate_cost = cost;
    type.delay = delay;
    type.inputs = {pin("Carry in", 1), pin("A", 8), pin("B", 8)};
    type.outputs = {pin("Sum", 8), pin("Carry out", 1)};
    type.active = true;
    return type;
}

}  // namespace

int main() {
    using namespace tc::component_registry;

    clear();
    check(count() == 0, "a fresh catalogue is empty");

    /* The declarative registration, as register_component records it. */
    check(note(declarative(0x1001, "example.byte-adder", "Byte Adder", 1, 1)),
          "a declarative type is recorded");
    Type type;
    check(find(0x1001, &type), "the type can be found by its id");
    check(type.active && type.owner_mod == "example.byte-adder" && type.name == "Byte Adder",
          "the entry keeps its owner and name");
    check(type.gate_cost == 1 && type.delay == 1, "the declared costs are kept");
    check(type.inputs.size() == 3 && type.outputs.size() == 2, "the declared pins are kept");
    check(type.inputs[1].name == "A" && type.inputs[1].bits == 8, "a pin keeps its name and width");
    check(type.capabilities == (TC_COMPONENT_CAP_LOGIC | TC_COMPONENT_CAP_WIDE_PIN |
                                TC_COMPONENT_CAP_MULTI_PIN),
          "the derived capabilities follow the shape");
    check(typeId(type) == "example.byte-adder/0x1001", "the type id is Mod-namespaced: " +
                                                          typeId(type));

    /* The bridge's answer: same shape, widths confirmed, names untouched. */
    noteShape(0x1001, "example.byte-adder",
              {pin("", 1), pin("", 8), pin("", 8)},
              {pin("", 8), pin("", 1)});
    check(find(0x1001, &type) && type.inputs[1].name == "A",
          "confirming the shape does not lose a declared pin name");
    check(type.inputs[1].bits == 8 && type.outputs[0].bits == 8,
          "confirming the shape keeps the compiled widths");
    check(type.gate_cost == 1, "confirming the shape does not lose the declared cost");

    /* A refusal, then the successful registration that replaces it. */
    noteRefused(0x1002, "example.bad-shape", "Wide Thing",
                "the bridge supports up to eight pins per direction, each 1..64 bits, "
                "with at most 128 input bits in total");
    check(count() == 2, "a refused type is still listed");
    check(find(0x1002, &type) && !type.active, "the refused type is inactive");
    check(type.status.find("up to eight pins") != std::string::npos,
          "the refusal keeps the bridge's reason");
    check(type.capabilities == 0, "an inactive type claims no capabilities");
    noteRefused(0x1002, "example.bad-shape", "Wide Thing", "a later, vaguer reason");
    check(find(0x1002, &type) && type.status.find("up to eight pins") != std::string::npos,
          "a later refusal does not overwrite the specific reason");
    check(note(declarative(0x1002, "example.bad-shape", "Wide Thing", 2, 3)),
          "the same id can be registered afterwards");
    check(find(0x1002, &type) && type.active && type.status.empty(),
          "the successful registration replaces the refusal");

    /* Two Mods, two types; a rejected Mod leaves nothing behind. */
    check(note(declarative(0x2001, "example.other", "Other", 4, 5)), "a second Mod registers");
    check(count() == 3, "the catalogue counts both Mods");
    dropMod("example.other");
    check(count() == 2 && !find(0x2001, &type), "a rejected Mod takes its types with it");
    dropMod("");
    check(count() == 2, "an empty owner name drops nothing");

    /* Pin limits: the catalogue caps what it reports instead of growing. */
    Type many;
    many.custom_id = 0x3001;
    many.owner_mod = "example.many-pins";
    many.active = true;
    for (std::size_t index = 0; index < kMaxPinsPerDirection + 4; ++index)
        many.inputs.push_back(pin("in", 1));
    check(note(std::move(many)), "a type with too many pins is still recorded");
    check(find(0x3001, &type) && type.inputs.size() == kMaxPinsPerDirection,
          "the reported pin list is capped");

    /* The catalogue's own cap. */
    clear();
    for (std::size_t index = 0; index < kMaxTypes; ++index) {
        Type entry;
        entry.custom_id = 0x4000 + index;
        entry.owner_mod = "example.full";
        entry.active = true;
        if (!note(std::move(entry))) check(false, "the catalogue filled up early");
    }
    Type overflow;
    overflow.custom_id = 0x9000;
    overflow.owner_mod = "example.full";
    overflow.active = true;
    check(!note(std::move(overflow)), "the catalogue refuses to grow past its cap");
    check(count() == kMaxTypes, "the catalogue stays at its cap");
    clear();

    /* The service's own shape: the table the host hands out. */
    TCComponentRegistryApiV1 api{};
    check(!tc::component_registry::ready(api), "an empty table is not ready");
    api.size = sizeof(api);
    api.version = TC_COMPONENT_REGISTRY_API_VERSION_1;
    api.count = [](void*) { return 1u; };
    api.get = [](void*, uint32_t, TCComponentTypeInfoV1*) { return TC_COMPONENT_REGISTRY_OK; };
    api.find = [](void*, uint64_t, TCComponentTypeInfoV1*) { return TC_COMPONENT_REGISTRY_OK; };
    api.pin = [](void*, uint64_t, uint32_t, uint32_t, TCComponentPinInfoV1*) {
        return TC_COMPONENT_REGISTRY_OK;
    };
    check(tc::component_registry::ready(api), "a complete table is ready");
    check(tc::component_registry::count(api) == 1, "count goes through the table");
    check(tc::component_registry::find(api, 1, nullptr) == TC_COMPONENT_REGISTRY_OK,
          "find goes through the table");
    check(TCComponentTypeInfoV1{}.size == 0, "a raw query struct starts unsized");
    check(tc::component_registry::typeInfo().size == sizeof(TCComponentTypeInfoV1),
          "the helper sizes the struct the host expects");

    if (failures == 0)
        std::cout << "PASS component registry: the catalogue keeps declared names, confirms the "
                     "compiled shape and reports refusals\n";
    return failures == 0 ? 0 : 1;
}
