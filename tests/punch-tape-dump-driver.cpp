/* Test-only driver: dump every component record of the current board, so the
   field a constant keeps its value in can be found by comparing the dump with
   what the drawer shows.  Nothing is written. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_handle_api.h"
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <string>

static const TCHost* host;
static bool done;
static int frames;

static void dumpComponent(uint64_t index, const uint8_t* component) {
    char header[128];
    std::snprintf(header, sizeof(header), "DUMP: component #%llu kind=0x%02x",
                  static_cast<unsigned long long>(index), component[0]);
    host->log(host->context, header);
    for (int row = 0; row < 0x120; row += 16) {
        char line[256];
        int used = std::snprintf(line, sizeof(line), "DUMP: +0x%03x:", row);
        for (int column = 0; column < 16; ++column) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02x", component[row + column]);
        }
        used += std::snprintf(line + used, sizeof(line) - used, "  ");
        for (int column = 0; column < 16; ++column) {
            const unsigned char c = component[row + column];
            used += std::snprintf(line + used, sizeof(line) - used, "%c",
                                  (c >= 32 && c < 127) ? c : '.');
        }
        host->log(host->context, line);
    }
}

static void frame(void*, const TCFrame*) {
    if (done) return;
    if (++frames < 300) return;
    TCGameHandle handle{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) return;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &handle, &raw) != TC_HANDLE_OK || !raw) return;
    auto* bytes = const_cast<uint8_t*>(static_cast<const uint8_t*>(raw));
    uint64_t count = 0;
    uint8_t* data = nullptr;
    memcpy(&count, bytes + 0x78, sizeof(count));
    memcpy(&data, bytes + 0x80, sizeof(data));
    if (!data || count == 0 || count > 40) return;
    done = true;
    char line[128];
    std::snprintf(line, sizeof(line), "DUMP: board components=%llu",
                  static_cast<unsigned long long>(count));
    host->log(host->context, line);
    for (uint64_t index = 0; index < count; ++index) dumpComponent(index, data + index * 0x238);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    out->on_frame = frame;
    h->log(h->context, "DUMP: armed");
    return 0;
}
