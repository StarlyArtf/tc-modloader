/* TC Mod Loader installer: entry point and the headless modes.

   docs/PLAN-installer-ui.md §6: the file work lives in setup_core, discovery in
   setup_discovery, the state rules in setup_state and the window in
   setup_window.  This file only decides between "install this directory and
   exit" (the mode the release tests and the elevation path use) and "open the
   installer window". */

#include "setup_core.hpp"
#include "setup_state.hpp"
#include "setup_window.hpp"
#include "version.hpp"

#include <shellapi.h>

#include <exception>
#include <string>

namespace {

/* Headless mode: no window, no dialog, one exit code.  --install /
   --uninstall keep their old argument shape and their old codes so the
   release tests and the elevated relaunch stay compatible. */
int run_headless(const tc::fs::path& root, bool remove) {
    const tc::setup::SetupResult result =
        remove ? tc::setup::uninstall(root) : tc::setup::install(root);
    OutputDebugStringW((result.user_message + L"\n").c_str());
    if (!result.technical_detail.empty())
        OutputDebugStringW((result.technical_detail + L"\n").c_str());
    return result.error == tc::setup::SetupError::none ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring directory;
    if (argv) {
        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index];
            if (argument == L"--install" || argument == L"--uninstall") {
                mode = argument;
                if (index + 1 < argc) directory = argv[++index];
            } else if (directory.empty()) {
                /* A bare directory opens the window on that location. */
                directory = argument;
            }
        }
        LocalFree(argv);
    }
    if (!mode.empty()) {
        if (directory.empty()) return 1;
        std::error_code error;
        const auto root = tc::fs::absolute(directory, error).lexically_normal();
        if (error) return 1;
        return run_headless(root, mode == L"--uninstall");
    }
    return tc::setup::run_window(instance, directory);
}
