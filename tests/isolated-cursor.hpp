#pragma once
#include <windows.h>
#include "../vendor/minhook/include/MinHook.h"

// Test-only pointer replay inside one cloned game. Buttons and wheel messages
// still traverse the game's backend; desktop clipping cannot change position.
namespace tc_test_cursor {
inline POINT position{};
inline bool positioned = false;
inline BOOL (WINAPI* original)(LPPOINT) = nullptr;
inline BOOL set(int x, int y) {
    position = POINT{x, y}; positioned = true; return TRUE;
}
inline BOOL WINAPI get(LPPOINT point) {
    if(positioned && point) { *point = position; return TRUE; }
    return original ? original(point) : FALSE;
}
inline bool install() {
    auto target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetCursorPos"));
    return target && MH_Initialize() == MH_OK &&
           MH_CreateHook(target, reinterpret_cast<void*>(get), reinterpret_cast<void**>(&original)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}
}
