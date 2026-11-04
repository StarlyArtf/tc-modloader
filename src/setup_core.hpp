#pragma once
/* The installer's file work: inspect (read-only), install and uninstall.

   docs/PLAN-installer-ui.md §5 and §6: this layer owns every decision that can
   change the game directory and never shows a dialog of its own.  It returns a
   SetupResult so the caller can pick words for a window, a log line or an exit
   code, and the caller cannot weaken the checks: install() re-reads the EXE,
   the engine and the backup on every call and refuses exactly what the old
   all-in-one installer refused. */

#include "core.hpp"
#include "setup_state.hpp"

#include <functional>
#include <string>

namespace tc::setup {

enum class SetupError {
    none,
    game_running,
    unsupported_exe,
    unsupported_engine,
    backup_missing,
    backup_invalid,
    permission_denied,
    io_error,
    verification_failed
};

/* The stages the window shows while a write is in flight.  There is no
   percentage: these operations are short and any number would be invented. */
enum class SetupPhase { rechecking, backing_up, deploying, verifying };

struct SetupResult {
    SetupError error = SetupError::none;
    std::wstring user_message;      /* one actionable Chinese line */
    std::wstring technical_detail;  /* paths, hashes and the raw reason */
    bool changed = false;           /* true once the game directory was touched */
};

using PhaseCallback = std::function<void(SetupPhase)>;

/* Read-only.  Safe to call from a worker thread; never throws. */
InstallationSnapshot inspect(const tc::fs::path& root, std::wstring source_label);

SetupResult install(const tc::fs::path& root, const PhaseCallback& phase = {});
SetupResult uninstall(const tc::fs::path& root, const PhaseCallback& phase = {});

/* True when a Turing Complete process runs from that exact directory.  An
   unreadable process list counts as "running" so a write is refused instead of
   racing the game. */
bool game_running(const tc::fs::path& root);

std::wstring describe(SetupError error);

/* UTF-8 -> UTF-16, for the paths and reasons the loader reports. */
std::wstring widen(const std::string& utf8);

}  // namespace tc::setup
