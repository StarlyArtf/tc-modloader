/* Offline checks for the built-in type catalogue behind TC_SERVICE_COMPONENT_COMPOSE
   (M4's first cut: the stable spelling of a game part, next to the kind byte it
   means in the running build).

   The reading half lives in the loader and needs the real prototype table, so
   the real-machine probe (tests/compose-probe.cpp) carries that.  What this file
   pins is the half that *is* pure logic, and which a wrong answer would make
   silently useless:
     * the spelling - `builtin/<name>`, `#2` for a name this build carries twice,
       `builtin/0x<kind>` for a prototype the game left unnamed - and that it is
       assigned deterministically in kind order;
     * the lookups: by kind, by exact spelling, by a case-insensitive unnumbered
       name, and the fact that a numbered spelling is never folded onto another;
     * the two counts the boot log reports;
     * the catalogue ABI: the string capacities the contract promises and the
       field order a Mod compiled against V1 relies on. */
#include "../src/builtin_catalogue.hpp"
#include "../sdk/tc_component_compose.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

tc::builtin_catalogue::Pin pin(int x, int y, uint32_t bits = 1, uint64_t raw = 1) {
    tc::builtin_catalogue::Pin value;
    value.x = x; value.y = y; value.bits = bits; value.wordSizeRaw = raw;
    return value;
}

/* A miniature catalogue shaped like the real one: ascending kinds, two names
   that repeat, one nameless prototype, one kind without declared inputs. */
std::vector<tc::builtin_catalogue::Entry> sample() {
    std::vector<tc::builtin_catalogue::Entry> entries;
    const auto add = [&entries](uint8_t kind, const char* name, bool inputs, bool shape) {
        tc::builtin_catalogue::Entry entry;
        entry.kind = kind;
        entry.name = name ? name : "";
        entry.hasShape = shape;
        if (inputs) entry.inputs.push_back(pin(-1, -1));
        entry.outputs.push_back(pin(2, 0));
        entries.push_back(std::move(entry));
    };
    add(0x0d, "Delay Line", false, true);
    add(0x2f, "Splitter", true, false);
    add(0x37, "Delay Line", false, true);
    add(0x63, "Splitter", true, false);
    add(0x7b, nullptr, false, false);
    tc::builtin_catalogue::assignTypeIds(entries);
    return entries;
}

void testSpelling() {
    using namespace tc::builtin_catalogue;
    check(makeTypeId("AND", 0x04, 1) == "builtin/AND", "a plain name spells builtin/<name>");
    check(makeTypeId("Delay Line", 0x37, 2) == "builtin/Delay Line#2",
          "a repeated name numbers from the second one");
    check(makeTypeId("", 0x7b, 1) == "builtin/0x7b",
          "a nameless prototype falls back to its kind byte");
    check(makeTypeId("", 0x04, 3) == "builtin/0x04",
          "the kind fallback is not numbered (kinds are unique)");

    const auto entries = sample();
    check(entries.size() == 5, "sample catalogue holds five kinds");
    check(entries[0].typeId == "builtin/Delay Line" && entries[2].typeId == "builtin/Delay Line#2",
          "the lower kind keeps the plain spelling");
    check(entries[1].typeId == "builtin/Splitter" && entries[3].typeId == "builtin/Splitter#2",
          "duplicate ranks follow kind order");
    check(entries[4].typeId == "builtin/0x7b", "the nameless kind is spelled by kind");

    /* assignTypeIds runs twice on the same input in the loader (id is filled in
       one pass, lookups are built right after); it must not drift. */
    auto again = entries;
    assignTypeIds(again);
    bool stable = true;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].typeId != again[i].typeId) stable = false;
    check(stable, "assignTypeIds is idempotent");
}

void testLookups() {
    using namespace tc::builtin_catalogue;
    const auto entries = sample();
    check(indexOfKind(entries, 0x37) == 2, "a kind finds its entry");
    check(indexOfKind(entries, 0x99) == -1, "an unknown kind is -1");
    check(indexOfTypeId(entries, "builtin/Splitter#2") == 3, "a numbered spelling is exact");
    check(indexOfTypeId(entries, "builtin/and") == -1, "an unknown name is -1");
    check(indexOfTypeId(entries, "AND") == -1, "a spelling without the prefix is refused");
    check(indexOfTypeId(entries, "builtin/0x7b") == 4, "the kind fallback is found exactly");

    auto named = sample();
    named[4].name = "AND";
    named[4].kind = 0x04;
    assignTypeIds(named);
    check(indexOfTypeId(named, "builtin/and") == 4, "an unnumbered name folds case");
    check(indexOfTypeId(named, "builtin/AND") == 4, "the exact spelling still wins");

    /* The numbered spelling must not be reachable by folding: only #1 answers to
       the bare name, otherwise "builtin/Splitter" would silently mean two
       different parts depending on which entry the loop reached first. */
    check(indexOfTypeId(entries, "builtin/splitter") == 1,
          "the bare name folds onto the unnumbered entry only");
    check(indexOfTypeId(entries, "builtin/splitter#3") == -1,
          "a wrong number is not folded onto anything");
}

void testNameEquals() {
    using namespace tc::builtin_catalogue;
    check(nameEquals("AND", "and"), "ASCII case folds");
    check(!nameEquals("AND", "AND2"), "length is compared first");
    check(!nameEquals("AND", "OR"), "different names differ");
    check(nameEquals("Delay Line", "delay line"), "spaces are preserved and folded");
    /* Only ASCII is folded: a name in another script is compared byte for byte,
       so a Mod cannot assume a locale-dependent fold. */
    check(nameEquals("\xE6\x88\x96", "\xE6\x88\x96"), "multi-byte names compare exactly");
    check(!nameEquals("\xE6\x88\x96", "\xE6\x88\x8E"), "a different multi-byte name differs");
}

void testCounts() {
    using namespace tc::builtin_catalogue;
    const auto entries = sample();
    check(countWithoutDeclaredInputs(entries) == 3,
          "the kinds without a declared input pin are counted");
    check(countWithShape(entries) == 2, "the kinds that carry a shape are counted");
}

void testAbi() {
    static_assert(sizeof(((TCBuiltinTypeInfoV1*)0)->type_id) == 80, "type_id capacity");
    static_assert(sizeof(((TCBuiltinTypeInfoV1*)0)->name) == 64, "name capacity");
    static_assert(sizeof(((TCBuiltinTypeInfoV1*)0)->description) == 192, "description capacity");
    static_assert(TC_BUILTIN_PIN_LIMIT == 32, "pin limit matches the registry's");
    /* The field order a Mod compiled against V1 relies on. */
    static_assert(offsetof(TCBuiltinTypeInfoV1, size) == 0, "info starts with size");
    static_assert(offsetof(TCBuiltinTypeInfoV1, type_id) > offsetof(TCBuiltinTypeInfoV1, pin_limit),
                  "strings come after the counters");
    static_assert(offsetof(TCBuiltinPinV1, size) == 0, "pin starts with size");
    static_assert(offsetof(TCBuiltinPinV1, word_size_raw) > offsetof(TCBuiltinPinV1, bits),
                  "the raw word size trails the decoded fields");
    static_assert(offsetof(TCComponentComposeApiV1, count) == 16, "table: size, version, context");
    static_assert(offsetof(TCComponentComposeApiV1, pin) > offsetof(TCComponentComposeApiV1, find_by_type_id),
                  "table: pin comes last");

    /* The wrapper's helper has to agree with the flags the loader sets. */
    TCBuiltinPinV1 decoded{};
    decoded.bits = 8;
    check(tc::component_compose::pinHasBits(decoded), "a numbered width has bits");
    decoded.flags = TC_BUILTIN_PIN_AUTO_WIDTH;
    decoded.bits = 0;
    check(!tc::component_compose::pinHasBits(decoded), "an auto marker has no bits");
    decoded.flags = TC_BUILTIN_PIN_RAW_WIDTH;
    decoded.bits = 0;
    check(!tc::component_compose::pinHasBits(decoded), "a raw marker has no bits");
}

}  // namespace

int main() {
    testSpelling();
    testLookups();
    testNameEquals();
    testCounts();
    testAbi();
    if (failures) {
        std::printf("component compose: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS component compose: spelling, lookups, counts and the catalogue ABI\n");
    return 0;
}
