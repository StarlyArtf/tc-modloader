/* WordWatchee 64 - keep all 64 bits in the value labels of wires and ports.

   Two independent defects drop the upper 32 bits:

   1. The renderer.  Renderer/multi_mesh/word_watchee_mesh.set_value_size writes
      the per-instance value_size byte after clamping it to 32 - the immediate
      of `mov eax,0x20` in

          mov  eax,0x20        B8 20 00 00 00
          mov  ecx,[rbp+0x14]  8B 4D 14
          mov  r9d,0x1         41 B9 01 00 00 00
          lea  r8,[rsp+0x24]   4C 8D 44 24 24
          cmp  bl,al           38 C3
          cmova ebx,eax        0F 47 D8

      (function + 0x40 in this build, VA 0x1402980f0).  Everything above 32 bits
      therefore reaches the vertex shader as the attribute value 32, so the
      shader's 33..64 bit paths are never taken and the label shows the low 32
      bits zero-extended.  The simulator is not the problem: the value is read
      as a full uint64 (sim_state_read_u64) and uploaded as 8 bytes.

   2. The shader.  asset/shader/word_watchee.vert, repr16()'s 64-bit branch
      starts the low word at `shift = 32` instead of 28, which emits one digit
      too many (17 for a 16-digit label) and shifts every low nibble by one
      place.  That one line is an exact text patch in this package's manifest,
      so the shader source stays the game's own file.

   This plugin raises the clamp to 64 and changes nothing else: boards whose
   word size is 1, 8 or 32 behave exactly as before, and a value_size the shader
   cannot represent (>64) still cannot reach it.  The bytes are verified before
   anything is written, so on a different game build the patch refuses to
   install and the game keeps its original behaviour. */
#include "tc_mod_api.h"
#include "tc_hook.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <cstring>
#include <fstream>

static const TCHost* host;

/* COFF name in this build, measured with tools/xref.js on the pinned EXE. */
static const char kSetValueSizeSymbol[] =
    "set_value_size__presenterZrendererZmulti95meshZword95watchee95mesh_u1008";

/* The window the patch rewrites the ceiling in, verbatim from the pinned EXE.
   tests/word-watchee-model.js re-reads these bytes from the executable and
   compares them with this table, so a stale copy fails the fast tier. */
static const uint8_t kClampBytes[] = {
    0xB8, 0x20, 0x00, 0x00, 0x00,  /* mov eax,0x20        value_size ceiling */
    0x8B, 0x4D, 0x14,              /* mov ecx,[rbp+0x14]  engine mesh handle   */
    0x41, 0xB9, 0x01, 0x00, 0x00, 0x00,  /* mov r9d,0x1   one byte written     */
    0x4C, 0x8D, 0x44, 0x24, 0x24,  /* lea r8,[rsp+0x24]   the byte's slot      */
    0x38, 0xC3,                    /* cmp bl,al                                */
    0x0F, 0x47, 0xD8,              /* cmova ebx,eax       min(value_size, eax) */
};
static const size_t kClampOffset = 0x40;      /* first byte of the window */
static const size_t kClampImmediate = 0x41;   /* the ceiling itself       */
static const uint8_t kCeiling32 = 0x20;
static const uint8_t kCeiling64 = 0x40;

/* The shader implements 1..64 bits (AUTO_WIDTH is at most 64); a larger
   value_size would shift a 32-bit word by 32 or more, which GLSL leaves
   undefined.  Raising the clamp to 64 is therefore the whole fix. */
static const int kCeilingBits = 64;

static void writeResultFile(const std::string& text) {
    if (!host->data_directory_utf8) return;
    std::ofstream out(std::string(host->data_directory_utf8) + "\\result.txt",
                      std::ios::binary | std::ios::trunc);
    if (out) out << text << "\n";
}

static std::string hex(uintptr_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

/* Optional development trace (TC_WATCHEE_TRACE=1): log the value_size the
   renderer is handed, so a player can see 64 arrive for a 64-bit board instead
   of taking the fix on faith.  Off unless the variable is set; the hook is the
   game's own function and no other mod hooks it. */
static void (*setValueSizeOriginal)(void*, uint32_t, uint32_t) = nullptr;
static int traceBudget = 0;
static void hookSetValueSize(void* mesh, uint32_t valueSize, uint32_t index) {
    if (traceBudget > 0) {
        --traceBudget;
        char line[160];
        std::snprintf(line, sizeof(line), "word-watchee-64: set_value_size(mesh=%p, size=%u, index=%u)",
                      mesh, valueSize, index);
        host->log(host->context, line);
    }
    if (setValueSizeOriginal) setValueSizeOriginal(mesh, valueSize, index);
}

static bool traceRequested() {
    return GetEnvironmentVariableW(L"TC_WATCHEE_TRACE", nullptr, 1) > 0;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != 1 || h->size < TC_HOST_BASE_SIZE || !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    /* create_hook sits past the base host size: a loader that only knows the
       base structure must be rejected here rather than read out of bounds. */
    if (!h->resolve_symbol || !h->log ||
        !tc::hostHasField(h, offsetof(TCHost, create_hook), sizeof(h->create_hook)) || !h->create_hook) return 2;

    unsigned char* code = static_cast<unsigned char*>(
        h->resolve_symbol(h->context, kSetValueSizeSymbol));
    if (!code) {
        tc::reportStatus(h, 2, "word-watchee-64: this game build has no word_watchee set_value_size symbol; the fix is not installed");
        return 3;
    }

    unsigned char* window = code + kClampOffset;
    std::string current;
    if (std::memcmp(window, kClampBytes, sizeof(kClampBytes)) == 0) {
        current = "mov eax,0x20";
    } else {
        /* Already patched by an earlier load in this process, or in the middle
           of the window by another tool: only the ceiling byte may differ. */
        uint8_t expected[sizeof(kClampBytes)];
        std::memcpy(expected, kClampBytes, sizeof(kClampBytes));
        expected[(kClampImmediate - kClampOffset)] = kCeiling64;
        if (std::memcmp(window, expected, sizeof(kClampBytes)) != 0) {
            std::string message = "word-watchee-64: unexpected code at ";
            message += hex(reinterpret_cast<uintptr_t>(window));
            message += " (expected the pinned build's clamp); nothing was patched";
            tc::reportStatus(h, 2, message.c_str());
            h->log(h->context, message.c_str());
            return 4;
        }
        current = "mov eax,0x40";
    }

    if (window[kClampImmediate - kClampOffset] != kCeiling64) {
        DWORD previous = 0;
        if (!VirtualProtect(window, sizeof(kClampBytes), PAGE_EXECUTE_READWRITE, &previous)) {
            tc::reportStatus(h, 2, "word-watchee-64: VirtualProtect failed; the value label stays 32-bit");
            return 5;
        }
        window[kClampImmediate - kClampOffset] = kCeiling64;
        DWORD ignored = 0;
        VirtualProtect(window, sizeof(kClampBytes), previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), window, sizeof(kClampBytes));
    }

    const uint8_t installed = window[kClampImmediate - kClampOffset];
    char line[256];
    std::snprintf(line, sizeof(line),
                  "word-watchee-64: value_size ceiling %u -> %u at %s (%s; read back %u)",
                  static_cast<unsigned>(kCeiling32), static_cast<unsigned>(kCeiling64),
                  hex(reinterpret_cast<uintptr_t>(window)).c_str(), current.c_str(),
                  static_cast<unsigned>(installed));
    h->log(h->context, line);
    writeResultFile(std::string("PASS symbol=") + hex(reinterpret_cast<uintptr_t>(code)) +
                    " window=" + hex(reinterpret_cast<uintptr_t>(window)) +
                    " before=" + current + " after=" + hex(installed) +
                    " ceiling=" + std::to_string(kCeilingBits));
    if (installed != kCeiling64) {
        tc::reportStatus(h, 2, "word-watchee-64: the ceiling could not be written; the value label stays 32-bit");
        return 6;
    }

    if (traceRequested()) {
        if (h->create_hook(h->context, code, reinterpret_cast<void*>(&hookSetValueSize),
                           reinterpret_cast<void**>(&setValueSizeOriginal)) == 0) {
            traceBudget = 64;
            h->log(h->context, "word-watchee-64: tracing the first 64 set_value_size calls");
        }
    }

    tc::reportStatus(h, 0, "word-watchee-64: value labels keep all 64 bits (ceiling 32 -> 64)");
    (void)out;
    return 0;
}
