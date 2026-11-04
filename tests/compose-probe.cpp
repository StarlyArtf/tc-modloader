/* Development-only real-machine probe for tc.component.compose V1, the built-in
   type catalogue (M4's first cut).

   It answers three questions in the loader's own log and in a dump file:
     1. does the service exist and how many kinds does it hold;
     2. are the lookups right - by kind, by the stable spelling, case-folded for
        the unnumbered name, refused for a numbered one that does not exist;
     3. does the catalogue agree, field by field, with the *other* reader of the
        same table.  That second reader is tests/kind-list-probe.cpp, which walks
        the prototype through the SDK's TCGameModel wrapper and writes
        build/kinds.txt; the playtest parses both files and requires them to
        match.  Two independent code paths over one table is the evidence here -
        either one silently drifting would show up as a mismatch.

   Read-only: it never mutates game state and asks for no level. */

#include "../sdk/tc_mod.h"
#include "../sdk/tc_component_compose.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const TCHost* host;
tc::component_compose::Api compose{};
bool done = false;
double startTime = 0.0;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::string hexByte(uint32_t value) {
    char text[8]{};
    std::snprintf(text, sizeof(text), "0x%02x", value & 0xffu);
    return text;
}

std::string pinText(uint32_t kind, uint32_t direction, uint32_t index) {
    TCBuiltinPinV1 pin{};
    pin.size = sizeof(pin);
    const int status = tc::component_compose::pin(compose, kind, direction, index, &pin);
    if (status != TC_COMPONENT_COMPOSE_OK) return "";
    std::ostringstream out;
    out << (direction == TC_COMPONENT_PIN_INPUT ? "in" : "out") << index << "=("
        << pin.x << "," << pin.y << ",w" << pin.word_size_raw << ")";
    return out.str();
}

/* One line per entry, in the same spelling kinds.txt uses for the fields both
   readers see, so the playtest can compare them without special cases. */
std::string dumpLine(const TCBuiltinTypeInfoV1& type, uint32_t* inputs, uint32_t* outputs) {
    std::ostringstream line;
    line << hexByte(type.kind) << " name=\"" << type.name << "\""
         << " inputs=" << type.input_count << " outputs=" << type.output_count;
    for (uint32_t i = 0; i < type.input_count && i < 8; ++i) {
        const std::string text = pinText(type.kind, TC_COMPONENT_PIN_INPUT, i);
        if (!text.empty()) line << " " << text;
    }
    for (uint32_t i = 0; i < type.output_count && i < 8; ++i) {
        const std::string text = pinText(type.kind, TC_COMPONENT_PIN_OUTPUT, i);
        if (!text.empty()) line << " " << text;
    }
    if (inputs) *inputs = type.input_count;
    if (outputs) *outputs = type.output_count;
    return line.str();
}

/* Looks one spelling up and reports what it found, so a failure names the
   spelling the Mod asked for rather than only an error code. */
bool expectTypeId(const char* spelling, uint32_t expectedKind, const char* expectedName) {
    TCBuiltinTypeInfoV1 type{};
    type.size = sizeof(type);
    const int status = tc::component_compose::findByTypeId(compose, spelling, &type);
    const bool ok = status == TC_COMPONENT_COMPOSE_OK && type.kind == expectedKind &&
                    std::string(type.name) == expectedName;
    log(std::string("compose spelling \"") + spelling + "\" status=" + std::to_string(status) +
        " kind=" + hexByte(type.kind) + " name=\"" + type.name + "\"" +
        (ok ? "" : "  <-- WRONG"));
    return ok;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    static bool started = false;
    if (!started) { started = true; startTime = value->time_seconds; }
    if (done || value->time_seconds - startTime < 3.0) return;
    done = true;
    const auto finish = [](const std::string& text) {
        const std::string folder = host->data_directory_utf8;
        std::ofstream(folder + "/result.txt") << text;
        host->log(host->context, text.c_str());
    };
    bool ok = true;
    const auto require = [&ok](bool condition, const char* what) {
        if (condition) return;
        ok = false;
        log(std::string("compose FAIL ") + what);
    };

    if (!tc::component_compose::table(host, &compose)) {
        finish("FAIL compose-probe: this loader has no tc.component.compose");
        return;
    }
    const uint32_t count = tc::component_compose::count(compose);
    log("compose count=" + std::to_string(count));
    require(count > 0, "the catalogue is empty");

    /* The whole table, plus the two counts the boot log publishes. */
    std::ostringstream report;
    report << "# built-in prototypes: " << count << "\n";
    uint32_t withoutInputs = 0, shaped = 0, nameless = 0;
    for (uint32_t index = 0; index < count; ++index) {
        TCBuiltinTypeInfoV1 type{};
        type.size = sizeof(type);
        const int status = tc::component_compose::get(compose, index, &type);
        if (status != TC_COMPONENT_COMPOSE_OK) {
            require(false, "get() failed inside the catalogue");
            continue;
        }
        report << dumpLine(type, nullptr, nullptr) << "\n";
        if (type.flags & TC_BUILTIN_FLAG_NO_DECLARED_INPUTS) ++withoutInputs;
        if (type.flags & TC_BUILTIN_FLAG_HAS_SHAPE) ++shaped;
        if (!type.name[0]) ++nameless;
    }
    log("compose without-declared-inputs=" + std::to_string(withoutInputs) +
        " shaped=" + std::to_string(shaped) + " nameless=" + std::to_string(nameless));

    /* A named digital part, checked pin by pin against the values the pin probe
       prints for the same kind (build/kinds.txt, kind 0x04). */
    TCBuiltinTypeInfoV1 andGate{};
    andGate.size = sizeof(andGate);
    const int andStatus = tc::component_compose::find(compose, 0x04u, &andGate);
    log("compose find kind=0x04 status=" + std::to_string(andStatus) + " name=\"" + andGate.name +
        "\" in=" + std::to_string(andGate.input_count) + " out=" +
        std::to_string(andGate.output_count) + " " +
        pinText(0x04u, TC_COMPONENT_PIN_INPUT, 0) + " " +
        pinText(0x04u, TC_COMPONENT_PIN_INPUT, 1) + " " +
        pinText(0x04u, TC_COMPONENT_PIN_OUTPUT, 0));
    require(andStatus == TC_COMPONENT_COMPOSE_OK && std::string(andGate.name) == "AND" &&
                andGate.input_count == 2 && andGate.output_count == 1,
            "kind 0x04 is not the two-input AND the pin probe reports");

    /* The stable spellings, including the two names this build repeats.  The
       ranks follow ascending kind order, which is what makes them stable. */
    require(expectTypeId("builtin/AND", 0x04u, "AND"), "builtin/AND");
    require(expectTypeId("builtin/Delay Line", 0x0du, "Delay Line"),
            "the first Delay Line kind");
    require(expectTypeId("builtin/Delay Line#2", 0x37u, "Delay Line"),
            "the second Delay Line kind");
    require(expectTypeId("builtin/and", 0x04u, "AND"), "the case-folded spelling");

    /* Every name this build repeats: the ranks must follow ascending kind order
       (first entry unnumbered, then #2, #3 ...) and each spelling has to resolve
       back to the kind it was built from.  Reporting the groups also documents
       how many parts share a name in this build, which is the thing a Mod cannot
       guess. */
    {
        std::map<uint32_t, std::string> spelling;
        std::map<std::string, std::vector<uint32_t>> groups;
        for (uint32_t index = 0; index < count; ++index) {
            TCBuiltinTypeInfoV1 type{};
            type.size = sizeof(type);
            if (tc::component_compose::get(compose, index, &type) != TC_COMPONENT_COMPOSE_OK) continue;
            spelling[type.kind] = type.type_id;
            groups[type.name].push_back(type.kind);
        }
        for (const auto& group : groups) {
            if (group.second.size() < 2 || group.first.empty()) continue;
            std::ostringstream kinds, ids;
            for (size_t rank = 0; rank < group.second.size(); ++rank) {
                if (rank) { kinds << ","; ids << ","; }
                char kindText[8]{};
                std::snprintf(kindText, sizeof(kindText), "0x%02x", group.second[rank] & 0xffu);
                kinds << kindText;
                ids << spelling[group.second[rank]];
                const std::string expected =
                    rank == 0 ? "builtin/" + group.first
                              : "builtin/" + group.first + "#" + std::to_string(rank + 1);
                require(spelling[group.second[rank]] == expected,
                        "a repeated name is not numbered in kind order");
                TCBuiltinTypeInfoV1 roundTrip{};
                roundTrip.size = sizeof(roundTrip);
                const int status =
                    tc::component_compose::findByTypeId(compose, spelling[group.second[rank]].c_str(),
                                                        &roundTrip);
                require(status == TC_COMPONENT_COMPOSE_OK && roundTrip.kind == group.second[rank],
                        "a spelled name does not resolve back to its own kind");
            }
            log("compose duplicates name=\"" + group.first + "\" kinds=" + kinds.str() +
                " ids=" + ids.str());
        }
    }

    /* The nameless prototypes: this build's table holds a good number of them
       (slots the game keeps without a name - 0x7b is the all-zero one).  Each is
       spelled by its kind, which is also the only way to ask for it, so the
       kind spelling has to work and must not invent a name. */
    {
        TCBuiltinTypeInfoV1 unnamed{};
        unnamed.size = sizeof(unnamed);
        const int status = tc::component_compose::findByTypeId(compose, "builtin/0x7b", &unnamed);
        log("compose nameless status=" + std::to_string(status) + " kind=" + hexByte(unnamed.kind) +
            " name=\"" + unnamed.name + "\" in=" + std::to_string(unnamed.input_count) +
            " out=" + std::to_string(unnamed.output_count));
        require(status == TC_COMPONENT_COMPOSE_OK && unnamed.kind == 0x7bu && !unnamed.name[0],
                "a nameless kind is not reachable by its kind spelling");
    }

    /* Refusals: an unknown kind, an unknown spelling, a pin past the end and a
       direction that is neither input nor output. */
    TCBuiltinTypeInfoV1 unknown{};
    unknown.size = sizeof(unknown);
    const int unknownKind = tc::component_compose::find(compose, 0x99u, &unknown);
    const int unknownId = tc::component_compose::findByTypeId(compose, "builtin/Nope", &unknown);
    TCBuiltinPinV1 pin{};
    pin.size = sizeof(pin);
    const int pinRange = tc::component_compose::pin(compose, 0x04u, TC_COMPONENT_PIN_INPUT, 9u, &pin);
    const int badDirection = tc::component_compose::pin(compose, 0x04u, 2u, 0u, &pin);
    TCBuiltinTypeInfoV1 small = unknown;
    small.size = sizeof(small) - 1;
    const int shortBuffer = tc::component_compose::get(compose, 0u, &small);
    log("compose refusals unknown-kind=" + std::to_string(unknownKind) +
        " unknown-id=" + std::to_string(unknownId) +
        " pin-range=" + std::to_string(pinRange) +
        " bad-direction=" + std::to_string(badDirection) +
        " short-buffer=" + std::to_string(shortBuffer));
    require(unknownKind == TC_COMPONENT_COMPOSE_ERR_UNKNOWN, "an unknown kind is not refused");
    require(unknownId == TC_COMPONENT_COMPOSE_ERR_UNKNOWN, "an unknown spelling is not refused");
    require(pinRange == TC_COMPONENT_COMPOSE_ERR_RANGE, "a pin past the end is not refused");
    require(badDirection == TC_COMPONENT_COMPOSE_ERR_ARGUMENT, "a bad direction is not refused");
    require(shortBuffer == TC_COMPONENT_COMPOSE_ERR_ARGUMENT,
            "an undersized output struct is not refused");

    std::ofstream(std::string(host->data_directory_utf8) + "/compose.txt") << report.str();
    finish(ok ? "PASS compose: the built-in catalogue answers by kind and by stable spelling, "
                "refuses what it should, and its dump is ready to be compared with the pin probe"
              : "FAIL compose: the built-in catalogue answered something else (see the log)");
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!tc::component_compose::table(h, &compose)) return 2;
    plugin->on_frame = frame;
    log("compose-probe: observing the built-in type catalogue");
    return 0;
}
