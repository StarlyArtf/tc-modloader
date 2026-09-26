// Development probe: dump the game's *own* component-menu tree.
//
// The right-hand component column is drawn from the mini tree the game keeps in
// the presenter context (`build__presenterZboard95uiZcomponent95menuZmini95tree_u2`
// walks it, and `reload_component_menu__presenterZutilities_u14192` fills it in
// through `get_component_menu__presenterZutilities_u9607`).  This probe reads
// that tree at the moment the game builds it, so the dump cannot be stale:
//
//   * it hooks `reload_component_menu__presenterZutilities_u14192` - a plain
//     EXE function, which the loader's hook.create accepts - and dumps the tree
//     *after* the game's own function ran (its second argument is the context
//     the menu lands in);
//   * the tree is `context+0xd948` (count) / `context+0xd950` (payload), and the
//     entries are the game's own menu nodes: byte +1 low three bits 2 = a
//     category, 1 = a component;
//   * every dereference goes through a VirtualQuery check, so a field that is
//     not what it looks like cannot fault the game.
//
// Read-only: the probe never writes game memory.  The dump lands in the
// plugin's data directory as `menu.txt`.

#include "../sdk/tc_mod.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <windows.h>

namespace {

const TCHost* host;
tc::TCGameModel game;
int dumps = 0;
double first_frame = -1.0;

void log(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

std::string hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

bool readable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto* begin = static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset =
        static_cast<size_t>(static_cast<const unsigned char*>(address) - begin);
    return offset + bytes <= region.RegionSize;
}

uint64_t readU64(const void* address) {
    uint64_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

const unsigned char* readPtr(const void* address) {
    const unsigned char* value = nullptr;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

/* A Nim string is {int64 length; char* payload}; the pinned build's strings
   carry an eight byte header before the characters. */
std::string nimString(const unsigned char* field) {
    if (!field || !readable(field, 16)) return "<unreadable>";
    const uint64_t length = readU64(field);
    const unsigned char* data = readPtr(field + 8);
    if (length == 0) return "\"\"";
    if (length > 256 || !readable(data, static_cast<size_t>(length) + 9))
        return "<not a string: len=" + std::to_string(length) + ">";
    return "\"" + std::string(reinterpret_cast<const char*>(data + 8),
                              static_cast<size_t>(length)) +
           "\"";
}

std::string prototypeNameFor(uint64_t id) {
    if (!game.valid() || !game.hasCustomPrototype(id)) return "<not a custom id>";
    tc::TCPrototype prototype{};
    if (!game.getCustomPrototype(id, prototype)) return "<unknown>";
    const char* name = tc::prototypeNameCStr(prototype);
    return name ? std::string("\"") + name + "\"" : "<none>";
}

/* One menu node: the layout the game's own mini tree and add_to_menu_tree code
   agree on - byte +1 is the variant tag, +8/+0x10 a name string, +0x20 a seq of
   children. */
void dumpNode(std::ostringstream& out, const unsigned char* node, int depth) {
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    if (!readable(node, 0x30)) {
        out << indent << "node " << hex(reinterpret_cast<uintptr_t>(node))
            << " is not readable\n";
        return;
    }
    const uint8_t tag = static_cast<uint8_t>(node[1] & 0x7);
    out << indent << "node=" << hex(reinterpret_cast<uintptr_t>(node))
        << " byte0=" << static_cast<int>(node[0]) << " byte1="
        << static_cast<int>(node[1]) << " tag=" << static_cast<int>(tag);
    if (tag == 2)
        out << " name=" << nimString(node + 8);
    else
        out << " id=" << hex(readU64(node + 8)) << " name="
            << prototypeNameFor(readU64(node + 8));
    out << "\n";
    if (tag != 2 || depth >= 4) return;
    const uint64_t children = readU64(node + 0x20);
    const unsigned char* payload = readPtr(node + 0x28);
    out << indent << "  children=" << children << " payload="
        << hex(reinterpret_cast<uintptr_t>(payload)) << "\n";
    if (children == 0) return;
    if (children > 4096 ||
        !readable(payload, static_cast<size_t>(children) * 8 + 16)) {
        out << indent << "  the child payload is not readable\n";
        return;
    }
    for (uint64_t index = 0; index < children && index < 256; ++index)
        dumpNode(out, readPtr(payload + 8 + index * 8), depth + 1);
}

void dumpContext(void* context, const char* why, bool writeFile) {
    if (!context) return;
    const auto* base = static_cast<const unsigned char*>(context);
    if (!readable(base + 0xd940, 0x40)) {
        log(std::string("menu-probe: ") + why +
            ": context+0xd940 is not readable");
        return;
    }
    const uint64_t count = readU64(base + 0xd948);
    const unsigned char* payload = readPtr(base + 0xd950);
    std::ostringstream report;
    report << "# the context the game's own reload_component_menu filled in\n"
           << "why=" << why << "\ncontext="
           << hex(reinterpret_cast<uintptr_t>(base)) << "\n"
           << "flag+0xd940=" << static_cast<int>(base[0xd940]) << "\n"
           << "categories count=" << count << " payload="
           << hex(reinterpret_cast<uintptr_t>(payload)) << "\n"
           << "selected+0xd990 count=" << readU64(base + 0xd990) << "\n"
           << "second list +0xd958=" << hex(readU64(base + 0xd958)) << "/"
           << hex(reinterpret_cast<uintptr_t>(readPtr(base + 0xd960))) << "\n";
    if (count > 0 && count < 4096 &&
        readable(payload, static_cast<size_t>(count) * 8 + 16)) {
        for (uint64_t index = 0; index < count; ++index)
            dumpNode(report, readPtr(payload + 8 + index * 8), 0);
    }
    const std::string text = report.str();
    if (writeFile)
        std::ofstream(std::string(host->data_directory_utf8) + "/menu.txt") << text;
    std::istringstream lines(text);
    std::string line;
    int emitted = 0;
    while (std::getline(lines, line) && emitted < 80) {
        log("menu-probe: " + line);
        ++emitted;
    }
}

using ReloadFn = void (*)(void*, void*, int);
ReloadFn originalReload = nullptr;

void detourReload(void* first, void* context, int flag) {
    if (originalReload) originalReload(first, context, flag);
    dumpContext(context, "after reload_component_menu", true);
    ++dumps;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (dumps > 0) return;
    if (first_frame < 0.0) first_frame = value->time_seconds;
    if (value->time_seconds - first_frame < 3.0) return;
    /* No reload happened yet: read whatever context the loader knows about, so
       the dump still says something instead of nothing. */
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
        return;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &board, &raw) != TC_HANDLE_OK || !raw) return;
    dumpContext(const_cast<void*>(raw), "board handle fallback", true);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!game.load(h) || !game.valid()) return 2;
    plugin->on_frame = frame;
    void* target = h->resolve_symbol
                       ? h->resolve_symbol(h->context,
                                           "reload_component_menu__presenterZutilities_u14192")
                       : nullptr;
    void* original = nullptr;
    const int status = (target && h->create_hook)
                           ? h->create_hook(h->context, target,
                                            reinterpret_cast<void*>(&detourReload), &original)
                           : -1;
    originalReload = reinterpret_cast<ReloadFn>(original);
    log(std::string("menu-probe: reload_component_menu=") +
        hex(reinterpret_cast<uintptr_t>(target)) + " hook=" + std::to_string(status) +
        " original=" + hex(reinterpret_cast<uintptr_t>(original)));
    return 0;
}
