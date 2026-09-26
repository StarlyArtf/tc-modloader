// Development probe: dumps the pinned build's built-in prototype table
// (kind, name, pin counts, word sizes and pin offsets) into the loader log and
// a text file, so fixtures and the SDK guide can be written against real data.
// Read-only: it never mutates game state.

#include "../sdk/tc_mod.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const TCHost* host;
tc::TCGameModel game;
bool done = false;
double start_time = 0.0;
double current_time = 0.0;
bool started = false;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

// Uses the public pin accessors only: the descriptor anchor and the word-size
// field are what they exist to hide.  Re-running this probe is therefore also a
// real-machine check of the wrapper - the dumped pin data must stay identical.
void readPin(const tc::TCPrototype& prototype, bool input, uint64_t index,
             std::ostringstream& out) {
    const bool present = input ? tc::prototypeInputPin(prototype, index) != nullptr
                               : tc::prototypeOutputPin(prototype, index) != nullptr;
    if (!present) return;
    const tc::TCPinPoint point = input ? tc::prototypeInputPinPoint(prototype, index)
                                       : tc::prototypeOutputPinPoint(prototype, index);
    const uint64_t word = input ? tc::prototypeInputPinWordSize(prototype, index)
                                : tc::prototypeOutputPinWordSize(prototype, index);
    out << " " << (input ? "in" : "out") << index << "=(" << point.x << "," << point.y
        << ",w" << word << ")";
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || current_time < 3.0) return;
    done = true;

    std::ostringstream report;
    const uint64_t count = game.builtinPrototypeCount();
    report << "# built-in prototypes: " << count << "\n";
    for (uint64_t index = 0; index < count; ++index) {
        const uint8_t kind = game.builtinPrototypeKindAt(index);
        tc::TCPrototype prototype{};
        if (!game.cloneBuiltinPrototype(kind, prototype)) continue;
        const char* name = tc::prototypeNameCStr(prototype);
        const uint64_t inputs = tc::prototypeInputCount(prototype);
        const uint64_t outputs = tc::prototypeOutputCount(prototype);
        std::ostringstream line;
        line << "0x" << std::hex << static_cast<int>(kind) << std::dec
             << " name=\"" << (name ? name : "") << "\""
             << " gates=" << tc::prototypeGateCost(prototype)
             << " delay=" << tc::prototypeDelay(prototype)
             << " inputs=" << inputs << " outputs=" << outputs;
        for (uint64_t i = 0; i < inputs && i < 4; ++i)
            readPin(prototype, true, i, line);
        for (uint64_t i = 0; i < outputs && i < 4; ++i)
            readPin(prototype, false, i, line);
        // Raw pointer/count fields around the pin lists.  Some stateful kinds
        // declare their ports through different slots, so dump them all.
        auto raw = [&](size_t offset) {
            uint64_t value = 0;
            memcpy(&value, prototype.bytes + offset, sizeof(value));
            return value;
        };
        line << " raw[0x58..0x80]=";
        for (size_t offset = 0x58; offset <= 0x80; offset += 8)
            line << " " << offset << ":" << raw(offset);
        report << line.str() << "\n";
    }

    const auto folder = std::string(host->data_directory_utf8);
    std::ofstream(folder + "/kinds.txt") << report.str();
    log("kind-list: wrote " + std::to_string(count) + " built-in prototypes");
    log(report.str());
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!game.load(h) || !game.valid()) return 2;
    plugin->on_frame = frame;
    log("kind-list: observing built-in prototype table");
    return 0;
}
