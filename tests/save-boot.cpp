/* The save redirect's decision table.

   The redirect patches the game's own save path literal in memory, which is
   build-specific.  Getting this wrong used to be fatal - the loader *is* the
   engine DLL, so refusing to load meant the game would not start at all - so the
   decision now lives in one pure function whose answers are pinned here:
   everything the loader cannot verify about the literal means "do not patch",
   and the caller then runs the session in the degraded mode instead of failing.

   Built and run by build.ps1 (see "Save redirect decision test"). */
#include "../src/save_boot.hpp"
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void expect(bool value, const char* what) {
    if (value) return;
    std::printf("FAIL %s\n", what);
    ++failures;
}
const char kBytes[32] = "Turing Complete";
}  // namespace

int main() {
    const void* expected = static_cast<const void*>(kBytes + 8);
    const void* payload = static_cast<const void*>(kBytes + 8);
    const char* text = "Turing Complete";
    // The pinned build's shape.
    expect(tc_save_boot::patchable(0x4a84b0, 15, payload, expected, kBytes, 16, text, 16),
           "the pinned literal should be patchable");
    expect(tc_save_boot::patchable(0x6000000, 15, payload, expected, kBytes, 16, text, 16),
           "a larger image is still patchable");
    // Every mismatch means "leave the player's save path alone".
    expect(!tc_save_boot::patchable(0x4a84af, 15, payload, expected, kBytes, 16, text, 16),
           "an image smaller than the expected layout must not be patched");
    expect(!tc_save_boot::patchable(0x6000000, 14, payload, expected, kBytes, 16, text, 16),
           "a different literal length must not be patched");
    expect(!tc_save_boot::patchable(0x6000000, 15, kBytes, expected, kBytes, 16, text, 16),
           "a literal that points somewhere else must not be patched");
    expect(!tc_save_boot::patchable(0x6000000, 15, payload, expected, "Turing CompletX", 16,
                                    text, 16),
           "different literal text must not be patched");
    expect(!tc_save_boot::patchable(0x6000000, 15, payload, expected, nullptr, 16, text, 16),
           "a missing payload must not be patched");
    expect(!tc_save_boot::patchable(0x6000000, 15, payload, expected, kBytes, 15, text, 16),
           "too few readable bytes must not be patched");
    if (failures) {
        std::printf("FAIL save redirect decision (%d)\n", failures);
        return 1;
    }
    std::printf("PASS save redirect decision: the pinned literal is patchable and every "
                "mismatch refuses instead of failing the load\n");
    return 0;
}
