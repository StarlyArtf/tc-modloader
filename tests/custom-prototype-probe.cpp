// Development probe: dump the game's own custom prototypes, whole.
//
// A custom component (kind 0x4e) is a *definition*: the game parses a circuit
// and keeps the result in its custom-prototype table.  simcore has to evaluate
// that definition to simulate the component, and the prototype is where the game
// keeps it - the reload path deep-copies "the parsed circuit data" and writes a
// 0x400-byte block into the prototype (docs/research/component-pipeline.md
// section 2).  This probe writes the raw bytes of every prototype named in
// TC_CUSTOM_DUMP so that layout can be located once, with the pins and the
// instance-level fields next to it as a check.
//
// Read-only: it never writes to the game, it only reads the table.

#include "../sdk/tc_mod.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const TCHost* host = nullptr;
tc::TCGameModel game;
bool done = false;
double started = 0.0;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::vector<uint64_t> requestedIds() {
    std::vector<uint64_t> ids;
    const char* text = std::getenv("TC_CUSTOM_DUMP");
    if (!text) return ids;
    std::string list(text);
    size_t at = 0;
    while (at < list.size()) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string piece = list.substr(at, comma - at);
        if (!piece.empty()) ids.push_back(std::strtoull(piece.c_str(), nullptr, 16));
        at = comma + 1;
    }
    return ids;
}

void frame(void*, const TCFrame* value) {
    if (!value) return;
    if (started == 0.0) started = value->time_seconds;
    if (done || value->time_seconds - started < 4.0) return;
    done = true;
    if (!game.load(host) || !game.valid()) {
        log("custom-prototype: the game model did not resolve");
        return;
    }
    const uint64_t table = game.customPrototypeCount();
    log("custom-prototype: the table holds " + std::to_string(table) + " prototype(s)");
    const std::string folder = host->data_directory_utf8 ? host->data_directory_utf8 : "";
    for (uint64_t id : requestedIds()) {
        tc::TCPrototype prototype{};
        if (!game.getCustomPrototype(id, prototype)) {
            log("custom-prototype: " + std::to_string(id) + " could not be read");
            continue;
        }
        const char* name = tc::prototypeNameCStr(prototype);
        const uint64_t inputs = tc::prototypeInputCount(prototype);
        const uint64_t outputs = tc::prototypeOutputCount(prototype);
        std::ostringstream head;
        head << "id=0x" << std::hex << id << std::dec << " name=\"" << (name ? name : "") << "\""
             << " inputs=" << inputs << " outputs=" << outputs;
        for (uint64_t index = 0; index < inputs && index < 8; ++index) {
            const tc::TCPinPoint point = tc::prototypeInputPinPoint(prototype, index);
            head << " in" << index << "=(" << point.x << "," << point.y << ",w"
                 << tc::prototypeInputPinWordSize(prototype, index) << ")";
        }
        for (uint64_t index = 0; index < outputs && index < 8; ++index) {
            const tc::TCPinPoint point = tc::prototypeOutputPinPoint(prototype, index);
            head << " out" << index << "=(" << point.x << "," << point.y << ",w"
                 << tc::prototypeOutputPinWordSize(prototype, index) << ")";
        }
        const std::string line = head.str();
        log("custom-prototype: " + line);
        if (folder.empty()) continue;
        char file[64]{};
        std::snprintf(file, sizeof(file), "\\custom-prototype-%016llx.txt",
                      static_cast<unsigned long long>(id));
        std::ofstream out(folder + file);
        out << "# " << line << "\n";
        /* The whole object, 16 bytes per line, so a nested circuit is visible
           as structure rather than as a wall of digits. */
        for (size_t offset = 0; offset < sizeof(prototype.bytes); offset += 16) {
            char bytes[80]{};
            char* at = bytes;
            for (size_t index = 0; index < 16 && offset + index < sizeof(prototype.bytes); ++index)
                at += std::snprintf(at, 4, " %02x", prototype.bytes[offset + index]);
            out << (offset < 0x100 ? "  " : offset < 0x1000 ? " " : "") << std::hex << offset << std::dec
                << ":" << bytes << "\n";
        }
        /* The two counted arrays at +0xe0 and +0x100 are the ones that scale
           with a definition's size (DEC: 15 and 20 entries; ALU: 29 and 109),
           so they are the definition's own component and wire lists.  Dump them
           through their pointers as well; the element layout is what the next
           step has to decode, and this is the data to decode it from. */
        const auto arrayHeader = [&](size_t offset) {
            uint64_t count = 0;
            const void* data = nullptr;
            std::memcpy(&count, prototype.bytes + offset, sizeof(count));
            std::memcpy(&data, prototype.bytes + offset + 8, sizeof(data));
            return std::make_pair(count, data);
        };
        const auto dumpArray = [&](std::ofstream& out, const char* label, uint64_t count,
                                   const void* data, size_t stride, size_t bytes_per) {
            out << "# " << label << " count=" << count << " ptr=" << data << " stride=" << stride << "\n";
            if (!data || count > 4096) return;
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (uint64_t index = 0; index < count; ++index) {
                const auto* entry = bytes + index * stride;
                out << label << "[" << index << "]";
                for (size_t at = 0; at < bytes_per && at < stride; ++at) {
                    char text[8]{};
                    std::snprintf(text, sizeof(text), " %02x", entry[at]);
                    out << text;
                }
                out << "\n";
            }
        };
        char definitionFile[64]{};
        std::snprintf(definitionFile, sizeof(definitionFile), "\\custom-definition-%016llx.txt",
                      static_cast<unsigned long long>(id));
        std::ofstream definition(folder + definitionFile);
        const auto components = arrayHeader(0xe0);
        const auto wires = arrayHeader(0x100);
        dumpArray(definition, "component", components.first, components.second, 0x238, 0x80);
        dumpArray(definition, "wire", wires.first, wires.second, 0x40, 0x40);
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    plugin->on_frame = frame;
    log("custom-prototype: armed");
    return 0;
}
