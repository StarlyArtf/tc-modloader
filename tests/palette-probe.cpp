// Development probe: what the game's component palette groups by.
//
// The right-hand palette shows a tab per category (BIT / WORD / MISC / IO /
// CUSTOM) and then the prototypes of the selected one.  The pinned build keeps
// that list in `CATEGORY_ORDER__modelZboardZprototype95list_u21` and every
// prototype carries a category value at +0x40, so this probe dumps both - plus
// the category every Float Ops component ended up with - and writes it to
// `palette.txt` in the plugin's data directory.  Read-only: it never mutates
// game state.

#include "../sdk/tc_mod.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

const TCHost* host;
tc::TCGameModel game;
bool done = false, started = false;
double start_time = 0.0;

/* The Float Ops identities, so the dump can say which category they landed in. */
const uint64_t kFloatOpsIds[] = {
    UINT64_C(0x463332434f4e5331), UINT64_C(0x4633324144445f31),
    UINT64_C(0x4633324449535031), UINT64_C(0x4633325355425f31),
    UINT64_C(0x4633324d554c5f31), UINT64_C(0x4633324449565f31),
    UINT64_C(0x4633325351545f31), UINT64_C(0x4633324e45475f31),
    UINT64_C(0x4633324142535f31), UINT64_C(0x463332434d505f31),
    UINT64_C(0x463332434c535f31), UINT64_C(0x463332464d415f31),
    UINT64_C(0x46333252454d5f31), UINT64_C(0x463332524e445f31),
    UINT64_C(0x4633324d494e5f31), UINT64_C(0x4633324d41585f31),
    UINT64_C(0x4633324932465f31), UINT64_C(0x4633325532465f31),
    UINT64_C(0x4633324632495f31), UINT64_C(0x4633324632555f31),
    UINT64_C(0x46333253504c5f31), UINT64_C(0x4633324d4b425f31),
};

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::string hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

/* Read one field of a prototype copy as a pointer. */
const void* fieldPointer(const tc::TCPrototype& prototype, size_t offset) {
    const void* value = nullptr;
    std::memcpy(&value, prototype.bytes + offset, sizeof(value));
    return value;
}

/* A Nim string is {int64 length; char* data} and the game's strings carry an
   8-byte header before the characters, the same layout the loader reads.  Only
   touched when the address is inside a committed page, so a field that is not a
   string cannot fault the probe. */
bool readable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto* begin = static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset = static_cast<size_t>(static_cast<const unsigned char*>(address) -
                                            begin);
    return offset + bytes <= region.RegionSize;
}

std::string pointedString(const void* address) {
    if (!readable(address, 16)) return "<unreadable>";
    int64_t length = 0;
    const char* data = nullptr;
    std::memcpy(&length, address, sizeof(length));
    std::memcpy(&data, static_cast<const char*>(address) + 8, sizeof(data));
    if (length <= 0 || length > 64 || !readable(data, static_cast<size_t>(length) + 9))
        return "<not a string: len=" + std::to_string(length) + ">";
    return std::string(data + 8, static_cast<size_t>(length));
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    if (done || value->time_seconds - start_time < 3.0) return;
    done = true;
    log("palette-probe: stage 1 (category order)");

    std::ostringstream report;

    /* The category table.  The symbol is a pointer *variable* (the code loads it
       through a .refptr cell and then reads 8-byte entries from `(%rax)`), so
       dereference it once and print the entries: the game's own code walks the
       first ten when it sorts components by category. */
    report << "# CATEGORY_ORDER pointer " << hex(reinterpret_cast<uintptr_t>(
                                                   game.category_order))
           << "\n";
    if (game.category_order) {
        /* The symbol is the table itself: the game's own code loads its address
           through a .refptr cell and then reads 8-byte entries from it, and each
           entry is a pointer to a category-name string. */
        const char* table = static_cast<const char*>(game.category_order);
        report << "# entries at " << hex(reinterpret_cast<uintptr_t>(table)) << "\n";
        if (readable(table, 10 * 8)) {
            for (int index = 0; index < 10; ++index) {
                const void* entry = nullptr;
                std::memcpy(&entry, table + index * 8, sizeof(entry));
                report << "category_order[" << index << "] ptr=" << hex(
                    reinterpret_cast<uintptr_t>(entry))
                       << " string=\"" << pointedString(entry) << "\"\n";
            }
        } else {
            report << "# the table is not readable as ten string pointers\n";
        }
    }

    /* Every built-in prototype's category, so the numbers above can be matched
       to the tabs a player sees. */
    report << "# built-in prototypes and their categories\n";
    log("palette-probe: stage 2 (built-in prototypes)");
    const uint64_t builtin = game.builtinPrototypeCount();
    for (uint64_t index = 0; index < builtin; ++index) {
        const uint8_t kind = game.builtinPrototypeKindAt(index);
        tc::TCPrototype prototype{};
        if (!game.cloneBuiltinPrototype(kind, prototype)) continue;
        report << "builtin kind=0x" << std::hex << static_cast<int>(kind) << std::dec
               << " category=" << tc::prototypeCategoryRaw(prototype)
               << " name=\"" << (tc::prototypeNameCStr(prototype) ?: "") << "\""
               << " menu+0x90=\"" << pointedString(fieldPointer(prototype, 0x90)) << "\""
               << " menu+0x98=\"" << pointedString(fieldPointer(prototype, 0x98)) << "\""
               << " menu+0x88=\"" << pointedString(fieldPointer(prototype, 0x88)) << "\""
               << "\n";
    }

    /* And the Float Ops components: the category the loader's registration gave
       them is the number the palette groups them under today. */
    report << "# Float Ops components\n";
    log("palette-probe: stage 3 (Float Ops prototypes)");
    for (uint64_t id : kFloatOpsIds) {
        /* A lookup for an id that is not registered has no guard inside the
           game's own accessor, so ask the membership set first. */
        if (!game.hasCustomPrototype(id)) {
            report << "float-ops id=" << hex(id) << " (not registered)\n";
            continue;
        }
        tc::TCPrototype prototype{};
        if (!game.getCustomPrototype(id, prototype)) {
            report << "float-ops id=" << hex(id) << " (not registered)\n";
            continue;
        }
        report << "float-ops id=" << hex(id)
               << " category=" << tc::prototypeCategoryRaw(prototype)
               << " kind=0x" << std::hex
               << static_cast<int>(static_cast<const uint8_t*>(prototype.bytes)[0])
               << std::dec << " name=\""
               << (tc::prototypeNameCStr(prototype) ?: "") << "\""
               << " menu+0x90=\"" << pointedString(fieldPointer(prototype, 0x90)) << "\""
               << " menu+0x98=\"" << pointedString(fieldPointer(prototype, 0x98)) << "\""
               << "\n";
    }

    const std::string folder = host->data_directory_utf8;
    std::ofstream(folder + "/palette.txt") << report.str();
    log("palette-probe: wrote the category dump");
    log(report.str());
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!game.load(h) || !game.valid()) return 2;
    plugin->on_frame = frame;
    log("palette-probe: observing the palette categories");
    return 0;
}
