/* Test-only probe for "let wide values use the game's own punch tape".

   The board UI's IO panel builds different widgets by pin width:

       cmp rdi,0x1 ; jle <one-bit branch>
       cmp rdi,0x8 ; jle <bit-toggle branch>    <- punch tape + value text
       ...                                      <- value text only

   The same shape appears twice more for the memory and register-file views
   (cmp rbx,0x8).  This probe raises the three ceilings to 64 in memory, after
   the loader has already validated the executable, so no file on disk changes.

   Measured on the pinned build (docs/research/punchcard-wide.md). */
#include "tc_mod_api.h"
#include <windows.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>

static const TCHost* host;

static const char kIoStateView[] =
    "build_io_state_view__presenterZboard95uiZio95state95view_u100";
static const uint8_t kCeiling8 = 0x08;
static const uint8_t kCeiling64 = 0x40;

struct PatchSite {
    uint32_t offset;      /* from the function start */
    uint8_t opcode[3];    /* the instruction that carries the ceiling */
};

static const PatchSite kSites[] = {
    {0x79d, {0x48, 0x83, 0xFF}},  /* cmp rdi,0x8  - level input/output pin */
    {0x1968, {0x48, 0x83, 0xFB}}, /* cmp rbx,0x8  - memory view            */
    {0x1c40, {0x48, 0x83, 0xFB}}, /* cmp rbx,0x8  - register file view     */
};

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != 1 || !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!h->resolve_symbol || !h->log) return 2;
    unsigned char* code = static_cast<unsigned char*>(h->resolve_symbol(h->context, kIoStateView));
    if (!code) {
        h->log(h->context, "punchcard probe: io state view symbol missing");
        return 3;
    }
    for (const PatchSite& site : kSites) {
        unsigned char* at = code + site.offset;
        if (std::memcmp(at, site.opcode, sizeof(site.opcode)) != 0 || at[3] != kCeiling8) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "punchcard probe: unexpected bytes at +0x%x (%02x %02x %02x %02x); not patched",
                          site.offset, at[0], at[1], at[2], at[3]);
            h->log(h->context, line);
            return 4;
        }
        DWORD previous = 0;
        if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &previous)) return 5;
        at[3] = kCeiling64;
        DWORD ignored = 0;
        VirtualProtect(at, 4, previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), at, 4);
        char line[160];
        std::snprintf(line, sizeof(line), "punchcard probe: ceiling 8 -> 64 at +0x%x", site.offset);
        h->log(h->context, line);
    }
    h->log(h->context, "punchcard probe: wide values now draw the game's punch tape");
    (void)out;
    return 0;
}
