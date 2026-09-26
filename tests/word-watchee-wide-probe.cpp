/* Test-only probe for the two-line wide label.  It hooks the renderer's
   word_watchee set_value_size and hands it 64 instead of the board's real word
   size, so an ordinary 8-bit board renders its labels through the >32-bit path:
   the top line shows the low 32 bits (the value itself), the bottom line the
   high bits (0).  Nothing else is touched - no game state, no save data - and
   the package never ships.

   Used by tests/word-watchee-layout-playtest.ps1, which compares the screenshot
   of a board with and without this probe. */
#include "tc_mod_api.h"
#include <windows.h>
#include <cwchar>

static const TCHost* host;
static void (*setValueSizeOriginal)(void*, uint32_t, uint32_t) = nullptr;
static void (*setValueOriginal)(void*, uint64_t, uint32_t) = nullptr;
static bool forceValue = false;
static uint64_t forcedValue = 0;

static void forceWide(void* mesh, uint32_t valueSize, uint32_t index) {
    (void)valueSize;
    if (setValueSizeOriginal) setValueSizeOriginal(mesh, 64u, index);
}

/* TC_WATCHEE_WIDE_VALUE=<decimal or 0x-hex> replaces every label's value, which
   is how the two-line split is checked against a value whose digits are known
   (default in the playtest: 2^64-1, twenty digits, and 2^63 as well). */
static void forceGiven(void* mesh, uint64_t value, uint32_t index) {
    if (forceValue) value = forcedValue;
    if (setValueOriginal) setValueOriginal(mesh, value, index);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != 1 || !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!h->resolve_symbol || !h->create_hook) return 2;
    void* target = h->resolve_symbol(
        h->context, "set_value_size__presenterZrendererZmulti95meshZword95watchee95mesh_u1008");
    if (!target) return 3;
    if (h->create_hook(h->context, target, reinterpret_cast<void*>(&forceWide),
                       reinterpret_cast<void**>(&setValueSizeOriginal)) != 0) return 4;
    void* valueTarget = h->resolve_symbol(
        h->context, "set_value__presenterZrendererZmulti95meshZword95watchee95mesh_u1269");
    if (valueTarget) {
        wchar_t text[64]{};
        const DWORD length = GetEnvironmentVariableW(L"TC_WATCHEE_WIDE_VALUE", text, 64);
        if (length > 0 && length < 64) {
            forcedValue = wcstoull(text, nullptr, 0);
            forceValue = true;
        }
        if (h->create_hook(h->context, valueTarget, reinterpret_cast<void*>(&forceGiven),
                           reinterpret_cast<void**>(&setValueOriginal)) != 0) forceValue = false;
    }
    h->log(h->context, "wide-label probe: every word label is drawn as 64 bits");
    return 0;
}
