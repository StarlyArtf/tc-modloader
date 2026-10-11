#pragma once
/* The built-in type catalogue behind `tc.component.compose` V1 (M4, first cut).

   A composite component is described with the game's own parts, and the game
   addresses those parts by kind byte.  A Mod must not hard-code a kind byte it
   read out of one build, so the catalogue gives every built-in prototype a
   *stable spelling* - `builtin/<name>`, with `#2`, `#3` ... when one build
   carries several kinds under the same name - next to the kind byte that
   spelling means in the running build.  The mapping itself is build data; a
   Mod that wants to survive a different build asks for the spelling.

   Everything in an entry was read from the prototype table the same way the
   board pin reads do it (src/board_pins.hpp).  Nothing here is inferred: a
   field the loader did not see is reported as absent (0 / flag), never guessed.
   The part that *is* pure logic - assigning and comparing those spellings - is
   what this header holds, so it can be tested offline (tests/component-compose.cpp).
   The reading side lives in src/native.hpp. */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace tc::builtin_catalogue {

/* One declared pin, in board cells relative to the node's own position - the
   same convention the component pin read (tc.board V5) reports, so a wire can
   be described without knowing anything about the circuit file format.

   `wordSizeRaw` is kept verbatim because this build uses more than one marker
   in that field: the plain auto-width marker `0x7fffffffffffffff` and, in the
   two splitters, values one and two below it.  `bits` therefore only carries a
   usable number when the raw value is 1..64; otherwise it is 0 and a flag says
   which case it is. */
struct Pin {
    int32_t x = 0, y = 0;
    uint32_t bits = 0;
    uint64_t wordSizeRaw = 0;
};

struct Entry {
    uint8_t kind = 0;
    std::string name, description, typeId;
    std::vector<Pin> inputs, outputs;
    bool hasShape = false;
};

/* The spelling a Mod asks for.  A nameless prototype (this build's table holds
   several unnamed slots) falls back to its kind byte; a name repeated in one
   build gets `#n`,
   counted in ascending kind order, which is the order the catalogue is built in
   (the boot enumeration hands kinds over sorted). */
inline std::string makeTypeId(const std::string& name, uint8_t kind, unsigned rank) {
    std::string base = "builtin/";
    if (name.empty()) {
        /* A nameless prototype is spelled by its kind byte, which is unique in
           the catalogue, so it never needs the duplicate suffix - not even when
           a future build carries more than one nameless kind. */
        char hex[8]{};
        std::snprintf(hex, sizeof(hex), "0x%02x", static_cast<unsigned>(kind));
        return base + hex;
    }
    base += name;
    if (rank > 1) base += "#" + std::to_string(rank);
    return base;
}

/* Fills in `typeId` for a whole catalogue, assuming the caller's order is the
   kind order (ascending).  Idempotent: re-running it on the same entries gives
   the same spellings. */
inline void assignTypeIds(std::vector<Entry>& entries) {
    std::map<std::string, unsigned> seen;
    for (auto& entry : entries)
        entry.typeId = makeTypeId(entry.name, entry.kind, ++seen[entry.name]);
}

/* ASCII case-insensitive comparison, so "and" finds the prototype the game
   spells "AND" without every Mod writing its own fold.  Only ASCII is folded;
   a name the game carries in another script is compared byte for byte. */
inline bool nameEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        const int foldedA = a < 0x80 ? std::tolower(a) : a;
        const int foldedB = b < 0x80 ? std::tolower(b) : b;
        if (foldedA != foldedB) return false;
    }
    return true;
}

/* Position of the entry with this kind, or -1.  The catalogue holds at most one
   entry per kind (the boot set is a set), so the answer is unique. */
inline int indexOfKind(const std::vector<Entry>& entries, uint32_t kind) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].kind == static_cast<uint8_t>(kind)) return static_cast<int>(i);
    return -1;
}

/* Position of the entry with this spelling, or -1.  First an exact match, then
   a case-insensitive one on the name part, so `builtin/and` finds `builtin/AND`
   while `builtin/Splitter#2` still means exactly the second splitter. */
inline int indexOfTypeId(const std::vector<Entry>& entries, const std::string& typeId) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].typeId == typeId) return static_cast<int>(i);
    constexpr const char* kPrefix = "builtin/";
    if (typeId.rfind(kPrefix, 0) != 0) return -1;
    const std::string wanted = typeId.substr(std::char_traits<char>::length(kPrefix));
    if (wanted.empty() || wanted.find('#') != std::string::npos) return -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name.empty()) continue;
        if (nameEquals(entries[i].name, wanted) && entries[i].typeId == "builtin/" + entries[i].name)
            return static_cast<int>(i);
    }
    return -1;
}

/* How many entries the prototype table declared no input pin for (the stateful
   families and the pure sources both land there - see
   docs/research/component-pipeline.md 11.2).  Reported as a count so the boot
   log can be read as "the same facts as last time" without a dump. */
inline size_t countWithoutDeclaredInputs(const std::vector<Entry>& entries) {
    size_t count = 0;
    for (const auto& entry : entries)
        if (entry.inputs.empty()) ++count;
    return count;
}

inline size_t countWithShape(const std::vector<Entry>& entries) {
    size_t count = 0;
    for (const auto& entry : entries)
        if (entry.hasShape) ++count;
    return count;
}

}  // namespace tc::builtin_catalogue
