#pragma once
/* The installer's state model, kept free of Windows and of the file system so
   that tests/setup-state.cpp can pin the whole matrix offline.

   docs/PLAN-installer-ui.md §4: the window never guesses a button from "does
   this file exist"; every location is reduced to an InstallationSnapshot and a
   pure rule turns that snapshot into the one main action, the one contextual
   secondary action and the `更多操作` menu.  A wrong rule is then a failing
   unit test instead of an install button that appears on an unknown engine. */

#include <filesystem>
#include <string>
#include <vector>

namespace tc::setup {

enum class Compatibility {
    checking,
    compatible,
    unsupported_exe,
    unsupported_engine,
    unreadable
};

enum class LoaderState {
    not_installed,
    installed_current,
    installed_previous,
    installed_unknown,
    backup_missing,
    backup_invalid
};

enum class Action {
    none,
    choose_location,
    install,
    repair,
    upgrade,
    retry,
    rescan,
    elevate,
    uninstall,
    confirm_uninstall,
    cancel,
    dismiss,
    open_directory,
    open_mods,
    open_log,
    technical,
    copy_technical
};

/* Everything the window may show about one location.  Produced by the read-only
   inspection in setup_core; nothing here is written by the inspection. */
struct InstallationSnapshot {
    std::filesystem::path root;
    std::wstring source_label;
    Compatibility compatibility = Compatibility::checking;
    LoaderState loader_state = LoaderState::installed_unknown;
    bool game_running = false;
    bool directory_writable = true;
    std::string exe_sha256;
    std::string engine_sha256;
    std::string backup_sha256;
    std::wstring summary;
    std::wstring technical_detail;
    std::wstring game_version;
};

struct MenuEntry {
    Action action = Action::none;
    std::wstring label;
    bool enabled = true;
};

struct ActionSet {
    Action primary = Action::none;
    std::wstring primary_label;
    bool primary_enabled = false;
    Action secondary = Action::none;
    std::wstring secondary_label;
    std::vector<MenuEntry> menu;
};

/* The state -> action rule.  Exactly one primary action, at most one visible
   secondary action, and a menu whose destructive entries are only offered when
   the snapshot proves they are safe. */
ActionSet actions_for(const InstallationSnapshot& snapshot);

/* Display text.  Kept out of the window so the same words are testable. */
std::wstring compatibility_text(const InstallationSnapshot& snapshot);
std::wstring loader_text(const InstallationSnapshot& snapshot);
/* The pinned game version from the generated compatibility profile. */
std::wstring game_version_text();
/* Two to three lines about this operation's safety, in a fixed order. */
std::vector<std::wstring> safety_lines(const InstallationSnapshot& snapshot);
/* Where the safety lines are drawn from: a color role, not a raw color. */
enum class LineTone { normal, good, warn };
std::vector<LineTone> safety_tones(const InstallationSnapshot& snapshot);

/* What the right-hand panel says after an operation finished. */
enum class ResultKind {
    none,
    installed,
    upgraded,
    repaired,
    uninstalled,
    nothing_changed,
    failed
};
std::wstring result_title(ResultKind kind);
bool result_is_success(ResultKind kind);

}  // namespace tc::setup
