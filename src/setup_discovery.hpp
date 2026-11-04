#pragma once
/* Where the installer looks for Turing Complete.

   docs/PLAN-installer-ui.md §5: explicit command line first, then Steam's
   current and extra libraries, then whatever the player added in this run,
   merged in that order, normalized and de-duplicated case-insensitively.  A
   Steam that cannot be read is not an installer error - it just means the
   player picks the directory.  Nothing here is persisted. */

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace tc::setup {

struct Candidate {
    std::filesystem::path root;
    std::wstring source;
};

/* Every double-quoted string of a Valve descriptor, in order, with the `\\`
   escapes undone.  Descriptors nest braces, so the tokens are the only shape a
   reader can rely on. */
std::vector<std::wstring> quoted_tokens(const std::wstring& text);

/* The library roots named by a libraryfolders.vdf: the value that follows each
   `"path"` key, plus the older clients' `"<index>" "<root>"` shape.  Nested
   blocks are skipped by looking at keys instead of pairing tokens up. */
std::vector<std::wstring> library_roots(const std::wstring& text);

/* Library roots from libraryfolders.vdf text, plus the Steam install's own
   steamapps, as `<library>/steamapps/common/Turing Complete`. */
std::vector<Candidate> steam_candidates(const std::wstring& steam_path,
                                        const std::wstring& library_folders_text);

/* HKCU\Software\Valve\Steam\SteamPath, or empty when Steam is not installed. */
std::wstring registry_steam_path();

bool looks_like_game(const std::filesystem::path& root);

/* Absolute, lexically normalized, de-duplicated without regard to case, in the
   order given.  Relative paths resolve against `base`. */
std::vector<Candidate> merge_candidates(const std::vector<Candidate>& candidates,
                                        const std::filesystem::path& base);

}  // namespace tc::setup
