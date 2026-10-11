#include "setup_discovery.hpp"

#include "compat.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace tc::setup {
namespace {

constexpr wchar_t kExeName[] = L"Turing Complete.exe";
constexpr wchar_t kEngineName[] = L"game_engine.dll";
constexpr wchar_t kGameFolder[] = L"Turing Complete";

std::wstring lowered(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return value;
}

std::wstring unescape(std::wstring value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'\\' && index + 1 < value.size() &&
            value[index + 1] == L'\\') {
            out.push_back(L'\\');
            ++index;
            continue;
        }
        out.push_back(value[index]);
    }
    return out;
}

bool looks_like_library(const std::wstring& value) {
    return value.find(L":\\") != std::wstring::npos ||
           value.find(L":/") != std::wstring::npos;
}

bool looks_like_key(const std::wstring& key) {
    if (lowered(key) == L"path") return true;
    /* Older clients used the library index as the key. */
    return !key.empty() &&
           key.find_first_not_of(L"0123456789") == std::wstring::npos;
}

}  // namespace

std::vector<std::wstring> quoted_tokens(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    std::wstring current;
    bool quoted = false;
    for (wchar_t c : text) {
        if (c == L'"') {
            if (quoted) tokens.push_back(unescape(current));
            current.clear();
            quoted = !quoted;
        } else if (quoted) {
            current.push_back(c);
        }
    }
    return tokens;
}

std::vector<std::wstring> library_roots(const std::wstring& text) {
    std::vector<std::wstring> roots;
    const auto tokens = quoted_tokens(text);
    for (size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (!looks_like_key(tokens[index])) continue;
        if (!looks_like_library(tokens[index + 1])) continue;
        if (std::find(roots.begin(), roots.end(), tokens[index + 1]) != roots.end())
            continue;
        roots.push_back(tokens[index + 1]);
    }
    return roots;
}

std::vector<Candidate> steam_candidates(const std::wstring& steam_path,
                                        const std::wstring& library_folders_text) {
    std::vector<Candidate> candidates;
    if (steam_path.empty()) return candidates;
    const std::filesystem::path steam(steam_path);
    candidates.push_back(
        {steam / L"steamapps" / L"common" / kGameFolder, L"Steam"});
    for (const auto& root : library_roots(library_folders_text)) {
        candidates.push_back(
            {std::filesystem::path(root) / L"steamapps" / L"common" / kGameFolder,
             L"Steam"});
    }
    return candidates;
}

std::wstring registry_steam_path() {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return value;
}

bool looks_like_game(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::exists(root / kExeName, error) &&
           std::filesystem::exists(root / kEngineName, error);
}

/* "C:\Games\TC" and "C:\Games\TC\" are the same location; a key that only
   lowercases would keep two rows for it.  The root of a drive keeps its
   separator. */
std::wstring place_key(const std::filesystem::path& root) {
    std::wstring key = lowered(root.wstring());
    while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    return key;
}

std::vector<Candidate> merge_candidates(const std::vector<Candidate>& candidates,
                                        const std::filesystem::path& base) {
    std::vector<Candidate> merged;
    std::vector<std::wstring> seen;
    for (const auto& candidate : candidates) {
        if (candidate.root.empty()) continue;
        std::error_code error;
        auto root = std::filesystem::absolute(
            candidate.root.is_absolute() ? candidate.root : base / candidate.root, error);
        if (error) continue;
        root = root.lexically_normal();
        if (!root.has_filename() && root.has_parent_path()) root = root.parent_path();
        const std::wstring key = place_key(root);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        merged.push_back({std::move(root), candidate.source});
    }
    return merged;
}

}  // namespace tc::setup
