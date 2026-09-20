/* Why the loader does not offer crash containment (see src/fault_guard.hpp).

   This probe is the "it works in isolation" half of that story: a vectored
   exception handler plus setjmp recovers from an access violation in all four
   shapes a plugin callback can have, and the process stays healthy afterwards.

     * same module as the handler,
     * behind a MinHook detour,
     * inside a separately loaded DLL (the helper next to this file),
     * behind a MinHook detour that then calls into that DLL.

   The fourth shape is exactly how a hook-chain link is invoked, yet the same
   mechanism does not survive inside the loader - the handler runs and the process
   still ends with STATUS_INVALID_IMAGE_HASH.  The measurements are in
   docs/verification.md; this file is what keeps them reproducible.

   Usage: fault-guard-probe <helper.dll>   (exit 0 = every shape recovered) */
#include <windows.h>
#include <MinHook.h>
#include <csetjmp>
#include <cstdio>
#include <string>

namespace {
thread_local jmp_buf* target = nullptr;
thread_local char message[128] = {};

LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    if (!target) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    snprintf(message, sizeof(message), "access violation");
    jmp_buf* jump = target;
    target = nullptr;
    longjmp(*jump, 1);
}
}  // namespace

static int crashHere(int depth) {
    std::string scratch(64, 'x');  // a non-trivial local, to show what leaks
    volatile int* nowhere = reinterpret_cast<volatile int*>(8);
    *nowhere = depth;
    return static_cast<int>(scratch.size());
}

namespace {
void (*hookedOriginal)() = nullptr;
void hookedDetour() { crashHere(1); }
/* Long enough for MinHook to place its jump; an empty function is not. */
__attribute__((noinline)) void hookedTarget() {
    volatile int counter = 0;
    for (int i = 0; i < 8; ++i) counter += i;
}

HMODULE helper = nullptr;
void (*helperOriginal)() = nullptr;
void helperDetour() {
    auto boom = helper ? reinterpret_cast<void (*)()>(GetProcAddress(helper, "tc_fault_guard_boom"))
                       : nullptr;
    if (boom) boom();
}
__attribute__((noinline)) void helperTarget() {
    volatile int counter = 0;
    for (int i = 0; i < 8; ++i) counter += i * 2;
}

/* Runs `call` and reports whether the guard brought us back. */
template <typename Call>
bool recovers(Call call) {
    jmp_buf jump;
    bool recovered = false;
    if (setjmp(jump) == 0) {
        target = &jump;
        call();
    } else {
        recovered = true;
    }
    target = nullptr;
    return recovered;
}

bool behindMinHook(void* targetFunction, void* detour, void** original, void (*call)()) {
    if (MH_CreateHook(targetFunction, detour, original) != MH_OK) return false;
    if (MH_EnableHook(targetFunction) != MH_OK) return false;
    return recovers(call);
}
}  // namespace

int main(int argc, char** argv) {
    if (MH_Initialize() != MH_OK) {
        printf("MinHook unavailable\n");
        return 2;
    }
    helper = argc > 1 ? LoadLibraryA(argv[1]) : nullptr;
    if (!helper) {
        printf("helper DLL missing (pass its path)\n");
        return 2;
    }
    AddVectoredExceptionHandler(1, handler);
    bool sameModule = recovers([] { crashHere(1); });
    bool viaMinHook = behindMinHook(reinterpret_cast<void*>(&hookedTarget),
                                    reinterpret_cast<void*>(&hookedDetour),
                                    reinterpret_cast<void**>(&hookedOriginal), &hookedTarget);
    bool inLoadedDll = recovers([] {
        auto boom = reinterpret_cast<void (*)()>(GetProcAddress(helper, "tc_fault_guard_boom"));
        if (boom) boom();
    });
    bool acrossBoth = behindMinHook(reinterpret_cast<void*>(&helperTarget),
                                    reinterpret_cast<void*>(&helperDetour),
                                    reinterpret_cast<void**>(&helperOriginal), &helperTarget);
    /* Still healthy? Ordinary work has to keep running after all that. */
    std::string after(8, 'y');
    printf("same-module=%d minhook=%d loaded-dll=%d cross-module=%d alive=%d message=%s\n",
           sameModule, viaMinHook, inLoadedDll, acrossBoth, static_cast<int>(after.size()), message);
    const bool ok = sameModule && viaMinHook && inLoadedDll && acrossBoth;
    if (ok)
        printf("PASS fault guard probe: all four shapes recovered in isolation (the loader cannot use "
               "this - see src/fault_guard.hpp)\n");
    return ok ? 0 : 1;
}
