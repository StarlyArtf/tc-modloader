/* The installer's state machine, pinned.

   docs/PLAN-installer-ui.md §4 and §10: the window must never derive a button
   from "does this file exist" by itself.  Every location becomes an
   InstallationSnapshot, and one pure rule turns it into the main action.  This
   test walks the whole matrix - including the states where a plausible-looking
   button would be destructive - plus the discovery rules (Steam library paths,
   normalization, case-insensitive de-duplication).

   Built and run by build.ps1 ("Installer state test"), and listed in
   tests/test-catalog.json. */

#include "../src/setup_discovery.hpp"
#include "../src/setup_state.hpp"
#include "../src/version.hpp"

#include <cstdio>
#include <string>

using namespace tc::setup;

namespace {

int failures = 0;

void expect(bool value, const char* what) {
    if (value) return;
    std::printf("FAIL %s\n", what);
    std::fflush(stdout);
    ++failures;
}

bool has(const ActionSet& set, Action action) {
    for (const auto& entry : set.menu)
        if (entry.action == action) return true;
    return false;
}

bool menu_enabled(const ActionSet& set, Action action) {
    for (const auto& entry : set.menu)
        if (entry.action == action) return entry.enabled;
    return false;
}

InstallationSnapshot snapshot(Compatibility compatibility, LoaderState loader,
                              bool running = false, bool writable = true) {
    InstallationSnapshot value;
    value.root = L"D:\\SteamLibrary\\steamapps\\common\\Turing Complete";
    value.source_label = L"Steam";
    value.compatibility = compatibility;
    value.loader_state = loader;
    value.game_running = running;
    value.directory_writable = writable;
    value.game_version = game_version_text();
    return value;
}

void test_actions() {
    /* Compatible, original engine: install. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::not_installed));
        expect(set.primary == Action::install, "an original engine installs");
        expect(set.primary_enabled, "the install button is offered");
        expect(set.primary_label == L"安装到此位置", "the install button is named");
        expect(set.secondary == Action::open_directory, "the secondary action opens the game");
        expect(!has(set, Action::uninstall), "nothing can be uninstalled before installing");
    }
    /* Installed, backup valid: repair, and the uninstall lives in the menu. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::installed_current));
        expect(set.primary == Action::repair, "a current install repairs");
        expect(set.primary_label == L"修复安装", "the repair button is named");
        expect(set.secondary == Action::open_mods, "the mods folder is one click away");
        expect(has(set, Action::uninstall), "uninstall is offered in the menu");
        expect(has(set, Action::open_log), "the log is offered in the menu");
        expect(set.secondary != Action::uninstall,
               "uninstall is never the equal-weight secondary button");
    }
    /* Known older loader: upgrade, named after the version being installed. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::installed_previous));
        expect(set.primary == Action::upgrade, "an old loader upgrades");
        expect(set.primary_label == L"升级到 " TC_MODLOADER_VERSION_WTEXT,
               "the upgrade button names the version it installs");
    }
    /* The game is running: nothing is written, and the button retries. */
    {
        const auto set = actions_for(
            snapshot(Compatibility::compatible, LoaderState::not_installed, true));
        expect(set.primary == Action::retry, "a running game only offers a retry");
        expect(set.primary_label == L"关闭游戏后重试", "the retry says what to do");
        expect(!has(set, Action::uninstall), "a running game offers no destructive action");
    }
    /* Every unusable state must refuse, in each of its shapes. */
    const struct {
        Compatibility compatibility;
        LoaderState loader;
        const char* what;
    } blocked[] = {
        {Compatibility::unsupported_exe, LoaderState::installed_unknown,
         "an unsupported executable offers no install"},
        {Compatibility::unsupported_engine, LoaderState::installed_unknown,
         "an unknown engine offers no install"},
        {Compatibility::unreadable, LoaderState::installed_unknown,
         "an unreadable directory offers no install"},
        {Compatibility::checking, LoaderState::not_installed,
         "a snapshot still being checked offers no install"},
        {Compatibility::compatible, LoaderState::backup_missing,
         "a missing backup offers no install"},
        {Compatibility::compatible, LoaderState::backup_invalid,
         "an invalid backup offers no install"},
        {Compatibility::compatible, LoaderState::installed_unknown,
         "an unknown engine offers no install"},
    };
    for (const auto& state : blocked) {
        const auto set = actions_for(snapshot(state.compatibility, state.loader));
        expect(!set.primary_enabled, state.what);
        expect(set.primary != Action::install && set.primary != Action::repair &&
                   set.primary != Action::upgrade && set.primary != Action::elevate,
               state.what);
        expect(!has(set, Action::uninstall) || !menu_enabled(set, Action::uninstall),
               "a blocked state never offers uninstall");
    }
    /* A missing or broken backup still allows looking at the facts. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::backup_invalid));
        expect(set.secondary == Action::technical,
               "a broken backup points at the technical detail");
        expect(has(set, Action::copy_technical), "the facts can be copied");
    }
    /* A read-only directory is the one case that asks for administrator rights. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::not_installed, false, false));
        expect(set.primary == Action::elevate, "a read-only directory elevates");
        expect(set.primary_label == L"使用管理员权限安装", "the elevated button says so");
    }
    /* Running beats read-only: closing the game is still the first step. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::not_installed, true, false));
        expect(set.primary == Action::retry, "a running game is closed before elevating");
    }
    /* A broken backup beats read-only: elevation cannot invent a backup. */
    {
        const auto set = actions_for(snapshot(Compatibility::compatible,
                                              LoaderState::backup_missing, false, false));
        expect(!set.primary_enabled, "elevation never overrides a missing backup");
    }
}

void test_text() {
    auto value = snapshot(Compatibility::compatible, LoaderState::installed_current);
    expect(loader_text(value) == L"已安装 " TC_MODLOADER_VERSION_WTEXT,
           "the loader row names the installed version");
    expect(compatibility_text(value) == L"兼容", "a compatible game reads as compatible");
    const auto lines = safety_lines(value);
    expect(lines.size() == 3, "a healthy install shows three safety lines");
    expect(lines[0].find(L"2.1.334") != std::wstring::npos,
           "the first safety line names the pinned game version");
    expect(lines[2].find(L"未运行") != std::wstring::npos,
           "the last safety line reports the process state");
    auto tones = safety_tones(value);
    expect(tones.size() == lines.size(), "every safety line carries a tone");
    /* An unsupported game must not claim that a backup is ready. */
    auto unsupported = snapshot(Compatibility::unsupported_exe, LoaderState::installed_unknown);
    const auto blocked_lines = safety_lines(unsupported);
    for (const auto& line : blocked_lines)
        expect(line.find(L"备份") == std::wstring::npos,
               "an unsupported executable says nothing about backups");
    expect(result_title(ResultKind::installed) ==
               L"Mod Loader " TC_MODLOADER_VERSION_WTEXT L" 已安装",
           "the success banner names the version");
    expect(result_is_success(ResultKind::uninstalled), "uninstalling is a success state");
    expect(!result_is_success(ResultKind::nothing_changed),
           "a cancelled elevation is not a success");
}

void test_discovery() {
    const std::wstring vdf =
        L"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files "
        L"(x86)\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t"
        L"\"D:\\\\SteamLibrary\"\n\t}\n}\n";
    const auto tokens = quoted_tokens(vdf);
    expect(tokens.size() == 9, "every quoted string of the descriptor is a token");
    expect(tokens.front() == L"libraryfolders", "the tokens keep their order");
    const auto roots = library_roots(vdf);
    expect(roots.size() == 2, "both library roots are found");
    expect(roots[0] == L"C:\\Program Files (x86)\\Steam", "the first library is unescaped");
    expect(roots[1] == L"D:\\SteamLibrary", "the second library is unescaped");
    const auto candidates = steam_candidates(L"C:\\Program Files (x86)\\Steam", vdf);
    expect(candidates.size() == 3, "the install's own library plus both library folders");
    expect(candidates[0].root ==
               std::filesystem::path(L"C:\\Program Files (x86)\\Steam") / L"steamapps" /
                   L"common" / L"Turing Complete",
           "the Steam install's own steamapps comes first");
    expect(candidates[1].root ==
               std::filesystem::path(L"C:\\Program Files (x86)\\Steam") / L"steamapps" /
                   L"common" / L"Turing Complete",
           "the library the client is installed in resolves to the same folder");
    expect(candidates[2].root ==
               std::filesystem::path(L"D:\\SteamLibrary") / L"steamapps" / L"common" /
                   L"Turing Complete",
           "the second library is kept");
    expect(steam_candidates(L"", vdf).empty(), "no Steam path means no candidates");
    /* The old client shape, and a nested "apps" block that must not shift the
       reading of the entries after it. */
    const std::wstring legacy =
        L"\"libraryfolders\"\n{\n\t\"1\"\t\t\"E:\\\\Games\\\\Steam\"\n\t\"2\"\n\t{\n"
        L"\t\t\"path\"\t\t\"F:\\\\Library\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"286160\"\t\t"
        L"\"4096\"\n\t\t}\n\t}\n}\n";
    const auto legacy_roots = library_roots(legacy);
    expect(legacy_roots.size() == 2, "the old and the new library shapes are both read");
    expect(legacy_roots[0] == L"E:\\Games\\Steam", "the indexed shape is read");
    expect(legacy_roots[1] == L"F:\\Library", "a nested apps block does not hide the path");

    /* Normalization: relative paths resolve, dots collapse, and the same
       location spelled differently is one row - the first spelling wins. */
    const auto merged = merge_candidates(
        {{L".\\sub\\..\\Turing Complete", L"手动添加"},
         {L"c:\\games\\TURING COMPLETE", L"命令行"},
         {L"C:\\Games\\Turing Complete\\", L"Steam"},
         {L"D:\\Other\\Turing Complete", L"Steam"},
         {L"", L"空"}},
        L"C:\\Games");
    expect(merged.size() == 2, "case and trailing separators do not create rows");
    expect(merged[0].root == std::filesystem::path(L"C:\\Games\\Turing Complete"),
           "a relative path resolves against the base and is normalized");
    expect(merged[0].source == L"手动添加", "the first spelling keeps its label");
    expect(merged[1].root == std::filesystem::path(L"D:\\Other\\Turing Complete"),
           "a different location is a different row");

    /* A directory only counts as a game when both files are there. */
    expect(looks_like_game(L"C:\\definitely\\not\\here") == false,
           "a directory without the game files is not a candidate");
}

}  // namespace

int main() {
    test_actions();
    test_text();
    test_discovery();
    if (failures) {
        std::printf("FAIL installer state (%d)\n", failures);
        return 1;
    }
    std::printf("PASS installer state: the action matrix, the safety text and the "
                "discovery rules are pinned\n");
    return 0;
}
