#pragma once
/* Fault journal: when a plugin's callback dies, say who and why.

   Containment was the original goal ("one bad pointer must not kill the game"),
   and it was prototyped with a vectored exception handler plus setjmp.  It works
   in isolation - see build/guard-probe.cpp, which survives an access violation in
   the same module, through a MinHook detour, inside a loaded DLL, and across both
   together - but *not* inside the loader: with the guard armed, the handler runs
   (this journal gets its line) and the process still terminates with
   STATUS_INVALID_IMAGE_HASH (0xC0000428).  That was measured twice: for a fault
   raised in a plugin DLL, and for one raised in the loader's own guarded block.

   A safety feature that works half the time is worse than none, so this file now
   does the part that is dependable: record *which plugin* and *which callback*
   was running when the fault happened, then let the exception continue.  The game
   crashes as it would have without the loader, and tc-modloader-data/fault.log
   names the culprit, which is what a bug report needs.  docs/verification.md has
   the measurements and what real containment would take. */
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace tc {
namespace fault {

inline HANDLE journal = INVALID_HANDLE_VALUE;
/* Marks the running plugin call.  Plain pointers to the plugin id and to a string
   literal, so setting and restoring them cannot allocate or fault by itself. */
inline thread_local const char* owner = nullptr;
inline thread_local const char* phase = nullptr;

inline void append(const char* text) {
    if (journal == INVALID_HANDLE_VALUE || !text) return;
    DWORD written = 0;
    WriteFile(journal, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    WriteFile(journal, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(journal);
}

inline bool reportable(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

inline const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        default: return "exception";
    }
}

inline LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    if (!reportable(code)) return EXCEPTION_CONTINUE_SEARCH;
    char address[40] = {};
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        snprintf(address, sizeof(address), " at 0x%p",
                 reinterpret_cast<const void*>(info->ExceptionRecord->ExceptionInformation[1]));
    char line[320];
    if (owner)
        snprintf(line, sizeof(line), "%s%s%s: %s%s", owner, phase ? " in " : "",
                 phase ? phase : "", exceptionName(code), address);
    else
        snprintf(line, sizeof(line), "outside any plugin callback: %s%s", exceptionName(code), address);
    append(line);
    /* Recorded, not handled: the process keeps the behaviour it would have had
       without the loader (see the measurement note above). */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Installed once per process; harmless when it is never needed. */
inline void install(const wchar_t* journalPath) {
    static bool done = false;
    if (done) return;
    done = true;
    if (journalPath)
        journal = CreateFileW(journalPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    AddVectoredExceptionHandler(1, handler);
}

/* Marks which plugin call is running and restores the previous mark on exit. */
class Scope {
public:
    Scope(const char* id, const char* what) : previousOwner_(owner), previousPhase_(phase) {
        owner = id;
        phase = what;
    }
    ~Scope() {
        owner = previousOwner_;
        phase = previousPhase_;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* previousOwner_;
    const char* previousPhase_;
};

}  // namespace fault
}  // namespace tc
