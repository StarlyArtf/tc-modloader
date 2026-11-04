#include "setup_core.hpp"

#include "compat.hpp"

#include <tlhelp32.h>

#include <cstdio>
#include <stdexcept>

namespace tc::setup {
namespace {

/* The installer addresses the game's files by name, which the loader writes
   wide-only; the names themselves come from the compatibility profile, so a
   renamed file fails the build here instead of silently looking for a file the
   game no longer has. */
constexpr bool same_name(const char* left, const char* right) {
    return *left == *right && (*left == '\0' || same_name(left + 1, right + 1));
}
static_assert(same_name(TC_GAME_EXE_NAME, "Turing Complete.exe"), "profile name drift");
static_assert(same_name(TC_GAME_ENGINE_NAME, "game_engine.dll"), "profile name drift");
static_assert(same_name(TC_GAME_ENGINE_BACKUP_NAME, "tc_game_engine.dll"),
              "profile name drift");

constexpr wchar_t kExeName[] = L"Turing Complete.exe";
constexpr wchar_t kEngineName[] = L"game_engine.dll";
constexpr wchar_t kBackupName[] = L"tc_game_engine.dll";

tc::fs::path exe_of(const tc::fs::path& root) { return root / kExeName; }
tc::fs::path engine_of(const tc::fs::path& root) { return root / kEngineName; }
tc::fs::path backup_of(const tc::fs::path& root) { return root / kBackupName; }

/* The embedded loader, read out of this executable's own resources. */
std::string payload() {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) throw std::runtime_error("Embedded loader is missing");
    HGLOBAL loaded = LoadResource(nullptr, resource);
    return std::string(static_cast<const char*>(LockResource(loaded)),
                       SizeofResource(nullptr, resource));
}

/* The standalone pin-name patch (dist/pin-names-patch) is a game_engine.dll of
   its own, built from the same source with TC_PIN_PATCH_ONLY.  It is not a
   loader, but it is a known occupant of that file, so the installer accepts it
   and replaces it instead of refusing with "another loader is installed".  The
   marker is looked for in the file - nothing of the patch is ever loaded. */
bool standalone_patch(const tc::fs::path& engine) {
    return tc::read(engine).find("tc_pin_patch_marker") != std::string::npos;
}

bool backup_present(const tc::fs::path& root) {
    return tc::fs::exists(backup_of(root));
}

bool backup_valid(const tc::fs::path& root) {
    if (!backup_present(root)) return false;
    return tc::hash(tc::read(backup_of(root))) == TC_ENGINE_SHA;
}

/* A refused or failed atomic replace can leave the staging file behind; the
   player should not have to guess what `game_engine.dll.tc-tmp` is. */
void clear_staging(const tc::fs::path& engine) {
    std::error_code error;
    tc::fs::remove(tc::fs::path(engine.wstring() + L".tc-tmp"), error);
}

/* A read-only check that the directory can be written to at all: create a
   throwaway file that deletes itself on close.  Nothing the game owns is
   touched, but the answer is the real ACL answer, which is what decides
   between the normal install button and the elevation one. */
bool directory_writable(const tc::fs::path& root) {
    const auto probe =
        root / (L".tc-modloader-write-probe-" + std::to_wstring(GetCurrentProcessId()));
    HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
}

/* One line per fact: the technical block shares its cell with the result
   banner, so a hash that wraps onto two lines pushes the rest out of sight.
   Labels stay short ("exe"/"引擎") because the panel is not that wide. */
std::wstring hash_line(const wchar_t* label, const std::string& value) {
    return std::wstring(label) + L"：" + (value.empty() ? L"(无)" : widen(value)) + L"\n";
}

std::wstring build_technical(const InstallationSnapshot& snapshot) {
    std::wstring detail = L"路径：" + snapshot.root.wstring() + L"\n";
    detail += hash_line(L"exe 实际 SHA-256", snapshot.exe_sha256);
    detail += hash_line(L"exe 期望 SHA-256", TC_EXE_SHA);
    detail += hash_line(L"引擎 实际 SHA-256", snapshot.engine_sha256);
    detail += hash_line(L"引擎 原版 SHA-256", TC_ENGINE_SHA);
    detail += hash_line(L"备份 SHA-256", snapshot.backup_sha256);
    detail += L"兼容配置：" + widen(TC_COMPAT_PROFILE_ID);
    detail += std::wstring(L"　游戏进程：") +
              (snapshot.game_running ? L"正在运行" : L"未运行");
    detail += std::wstring(L"　目录可写：") + (snapshot.directory_writable ? L"是" : L"否") +
              L"\n";
    return detail;
}

bool classify(const tc::fs::path& root, const std::string& engine_sha,
              const std::string& deployed, InstallationSnapshot& snapshot) {
    const bool patch = standalone_patch(engine_of(root));
    const bool has_backup = backup_present(root);
    const bool valid_backup = has_backup && backup_valid(root);
    snapshot.backup_sha256 = has_backup ? tc::hash(tc::read(backup_of(root))) : "";
    if (engine_sha == TC_ENGINE_SHA) {
        snapshot.loader_state = LoaderState::not_installed;
        snapshot.summary = L"原始引擎可安全备份。";
        return true;
    }
    if (engine_sha == deployed) {
        if (valid_backup) {
            snapshot.loader_state = LoaderState::installed_current;
            snapshot.summary = L"当前加载器已安装，原始引擎备份有效。";
        } else if (has_backup) {
            snapshot.loader_state = LoaderState::backup_invalid;
            snapshot.summary = L"原始引擎备份与预期不符，已禁止覆盖。";
        } else {
            snapshot.loader_state = LoaderState::backup_missing;
            snapshot.summary = L"找不到原始引擎备份，已禁止覆盖。";
        }
        return true;
    }
    /* Every loader a published release has left on a player's disk.  The list is
       compat/profiles.json's acceptedInstalledLoaders (the macros come from
       src/compat.generated.hpp), and it has to grow with every release: a
       loader that is missing here is read as installed_unknown and *refused*
       rather than replaced, which would strand the players who installed the
       previous release.  Measured 2026-09-28: the 0.6.0 and 0.8.0 installers
       deploy 4487399 and 5211697 byte loaders whose hashes were not in this
       list, so a 0.8.0 player could not upgrade to 0.8.1. */
    static const char* const accepted[] = {
        TC_LOADER_020_SHA, TC_LOADER_060_SHA, TC_LOADER_080_SHA, TC_LOADER_081_SHA,
        TC_PREVIOUS_LOADER_SHA, TC_LOADER_JIT_PREVIEW_SHA};
    bool known = patch;
    for (const char* sha : accepted) {
        if (engine_sha == sha) { known = true; break; }
    }
    if (known) {
        if (valid_backup) {
            snapshot.loader_state = LoaderState::installed_previous;
            snapshot.summary = L"检测到可安全升级的已知旧版本。";
        } else if (has_backup) {
            snapshot.loader_state = LoaderState::backup_invalid;
            snapshot.summary = L"旧版本备份校验失败，已禁止覆盖。";
        } else {
            snapshot.loader_state = LoaderState::backup_missing;
            snapshot.summary = L"旧版本没有原始引擎备份，已禁止覆盖。";
        }
        return true;
    }
    snapshot.loader_state = LoaderState::installed_unknown;
    snapshot.summary = L"game_engine.dll 已被其他程序修改，安装器不会覆盖它。";
    return false;
}

struct Inspection {
    InstallationSnapshot snapshot;
    SetupError error = SetupError::none;
};

/* The shared body of inspect() and of the pre-write check inside install() and
   uninstall(): the same reads, the same rules, one implementation. */
Inspection analyze(const tc::fs::path& root, std::wstring source_label) {
    Inspection inspection;
    auto& snapshot = inspection.snapshot;
    snapshot.root = tc::fs::absolute(root).lexically_normal();
    snapshot.source_label = std::move(source_label);
    snapshot.game_version = game_version_text();
    std::wstring note;
    try {
        if (!tc::fs::exists(exe_of(snapshot.root)) ||
            !tc::fs::exists(engine_of(snapshot.root))) {
            snapshot.compatibility = Compatibility::unreadable;
            snapshot.summary = L"目录中缺少游戏程序或 game_engine.dll。";
            inspection.error = SetupError::io_error;
        } else {
            snapshot.game_running = game_running(snapshot.root);
            snapshot.directory_writable = directory_writable(snapshot.root);
            snapshot.exe_sha256 = tc::hash(tc::read(exe_of(snapshot.root)));
            snapshot.engine_sha256 = tc::hash(tc::read(engine_of(snapshot.root)));
            if (snapshot.exe_sha256 != TC_EXE_SHA) {
                snapshot.compatibility = Compatibility::unsupported_exe;
                snapshot.loader_state = LoaderState::installed_unknown;
                snapshot.summary = L"这份游戏与当前兼容配置不匹配，安装器不会修改它。";
                inspection.error = SetupError::unsupported_exe;
            } else {
                const std::string deployed = tc::hash(payload());
                if (classify(snapshot.root, snapshot.engine_sha256, deployed, snapshot)) {
                    snapshot.compatibility = Compatibility::compatible;
                } else {
                    snapshot.compatibility = Compatibility::unsupported_engine;
                    inspection.error = SetupError::unsupported_engine;
                }
                if (snapshot.compatibility == Compatibility::compatible) {
                    switch (snapshot.loader_state) {
                        case LoaderState::backup_missing:
                            inspection.error = SetupError::backup_missing;
                            break;
                        case LoaderState::backup_invalid:
                            inspection.error = SetupError::backup_invalid;
                            break;
                        default: break;
                    }
                    if (inspection.error == SetupError::none && snapshot.game_running) {
                        inspection.error = SetupError::game_running;
                        note = L"游戏正在运行，请关闭后重试。";
                        snapshot.summary += L" " + note;
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        snapshot.compatibility = Compatibility::unreadable;
        snapshot.loader_state = LoaderState::installed_unknown;
        snapshot.summary = L"检查这份游戏时出错，安装器没有改动它。";
        note = L"异常：" + widen(error.what());
        inspection.error = SetupError::io_error;
    }
    snapshot.technical_detail = build_technical(snapshot);
    if (!note.empty()) snapshot.technical_detail += note + L"\n";
    return inspection;
}

SetupResult failure(SetupError error, const std::wstring& detail) {
    SetupResult result;
    result.error = error;
    result.user_message = describe(error);
    result.technical_detail = detail;
    return result;
}

/* Every write goes through this: refuse anything the fresh read does not
   approve, so a stale window state can never overwrite an engine. */
SetupError gate(const Inspection& inspection, bool removing) {
    if (inspection.snapshot.compatibility == Compatibility::unsupported_exe)
        return SetupError::unsupported_exe;
    if (inspection.snapshot.compatibility == Compatibility::unsupported_engine)
        return SetupError::unsupported_engine;
    if (inspection.snapshot.compatibility != Compatibility::compatible)
        return inspection.error == SetupError::none ? SetupError::io_error : inspection.error;
    if (inspection.snapshot.game_running) return SetupError::game_running;
    if (!inspection.snapshot.directory_writable) return SetupError::permission_denied;
    if (!removing) {
        switch (inspection.snapshot.loader_state) {
            case LoaderState::backup_missing: return SetupError::backup_missing;
            case LoaderState::backup_invalid: return SetupError::backup_invalid;
            default: break;
        }
    } else {
        switch (inspection.snapshot.loader_state) {
            case LoaderState::backup_missing: return SetupError::backup_missing;
            case LoaderState::backup_invalid: return SetupError::backup_invalid;
            case LoaderState::not_installed: return SetupError::none;
            default: break;
        }
    }
    return SetupError::none;
}

}  // namespace

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), count);
    if (!result.empty()) result.pop_back();
    return result;
}

std::wstring describe(SetupError error) {
    switch (error) {
        case SetupError::none: return L"没有需要报告的问题。";
        case SetupError::game_running:
            return L"检测到游戏仍在运行，请关闭后重试。";
        case SetupError::unsupported_exe:
            return L"这份游戏不是受支持的构建，安装器没有改动它。";
        case SetupError::unsupported_engine:
            return L"game_engine.dll 已被其他程序修改，安装器不会覆盖它。";
        case SetupError::backup_missing:
            return L"找不到原始引擎备份，安装器不会覆盖当前引擎。";
        case SetupError::backup_invalid:
            return L"原始引擎备份校验失败，安装器不会覆盖当前引擎。";
        case SetupError::permission_denied:
            return L"游戏目录不可写，需要管理员权限才能继续。";
        case SetupError::io_error:
            return L"读写游戏目录时出错，请查看技术信息。";
        case SetupError::verification_failed:
            return L"写入结果与预期不符，请查看技术信息。";
    }
    return L"安装器遇到未知问题。";
}

bool game_running(const tc::fs::path& root) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kExeName)) continue;
            HANDLE process =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                running = true;
                continue;
            }
            wchar_t path[32768]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size) &&
                tc::fs::path(path).parent_path() == root)
                running = true;
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

InstallationSnapshot inspect(const tc::fs::path& root, std::wstring source_label) {
    return analyze(root, std::move(source_label)).snapshot;
}

SetupResult install(const tc::fs::path& root, const PhaseCallback& phase) {
    if (phase) phase(SetupPhase::rechecking);
    Inspection inspection;
    try {
        inspection = analyze(root, {});
    } catch (const std::exception& error) {
        return failure(SetupError::io_error, widen(error.what()));
    }
    if (const SetupError blocked = gate(inspection, false); blocked != SetupError::none)
        return failure(blocked, inspection.snapshot.technical_detail);
    SetupResult result;
    try {
        const auto directory = inspection.snapshot.root;
        const auto engine = engine_of(directory);
        const auto backup = backup_of(directory);
        tc::no_links(directory, exe_of(directory));
        tc::no_links(directory, engine);
        tc::no_links(directory, backup);
        const auto bytes = payload();
        const auto deployed = tc::hash(bytes);
        if (!backup_present(directory)) {
            if (phase) phase(SetupPhase::backing_up);
            if (!CopyFileW(engine.c_str(), backup.c_str(), TRUE))
                return failure(SetupError::io_error,
                               inspection.snapshot.technical_detail +
                                   L"无法创建原始引擎备份。\n");
            result.changed = true;
        }
        if (phase) phase(SetupPhase::deploying);
        tc::no_links(directory, directory / L"mods");
        tc::fs::create_directories(directory / L"mods");
        tc::atomic(engine, bytes);
        result.changed = true;
        if (phase) phase(SetupPhase::verifying);
        if (tc::hash(tc::read(engine)) != deployed) {
            clear_staging(engine);
            return failure(SetupError::verification_failed,
                           inspection.snapshot.technical_detail +
                               L"写入后校验 game_engine.dll 与加载器不一致。\n");
        }
        if (tc::hash(tc::read(backup)) != TC_ENGINE_SHA) {
            clear_staging(engine);
            return failure(SetupError::verification_failed,
                           inspection.snapshot.technical_detail +
                               L"写入后校验原始引擎备份不一致。\n");
        }
    } catch (const std::exception& error) {
        clear_staging(engine_of(inspection.snapshot.root));
        SetupResult failed = failure(SetupError::io_error, widen(error.what()));
        failed.changed = result.changed;
        return failed;
    }
    result.user_message =
        L"Mod Loader " TC_MODLOADER_VERSION_WTEXT
        L" 已安装\n原始引擎已安全备份\n\n把 .mod 文件放入 Mods 文件夹，然后正常启动游戏，"
        L"在主菜单 Mods 页面启用。";
    return result;
}

SetupResult uninstall(const tc::fs::path& root, const PhaseCallback& phase) {
    if (phase) phase(SetupPhase::rechecking);
    Inspection inspection;
    try {
        inspection = analyze(root, {});
    } catch (const std::exception& error) {
        return failure(SetupError::io_error, widen(error.what()));
    }
    if (const SetupError blocked = gate(inspection, true); blocked != SetupError::none)
        return failure(blocked, inspection.snapshot.technical_detail);
    const auto directory = inspection.snapshot.root;
    if (inspection.snapshot.loader_state == LoaderState::not_installed &&
        !backup_present(directory)) {
        SetupResult result;
        result.user_message = L"这份游戏没有安装 Mod Loader，未进行任何更改。";
        return result;
    }
    SetupResult result;
    try {
        const auto engine = engine_of(directory);
        const auto backup = backup_of(directory);
        tc::no_links(directory, exe_of(directory));
        tc::no_links(directory, engine);
        tc::no_links(directory, backup);
        const auto deployed = tc::hash(payload());
        const bool patch = standalone_patch(engine);
        const auto actual = tc::hash(tc::read(engine));
        // analyze() recognizes released predecessors by their pinned hashes.
        // Recheck the bytes here so a change after inspection is still refused.
        const bool previous = inspection.snapshot.loader_state == LoaderState::installed_previous &&
                              actual == inspection.snapshot.engine_sha256;
        if (actual != deployed && !patch && !previous)
            return failure(SetupError::unsupported_engine,
                           inspection.snapshot.technical_detail +
                               L"当前引擎不是已识别的加载器或独立补丁，拒绝覆盖。\n");
        if (!backup_valid(directory))
            return failure(backup_present(directory) ? SetupError::backup_invalid
                                                     : SetupError::backup_missing,
                           inspection.snapshot.technical_detail);
        if (phase) phase(SetupPhase::deploying);
        /* Restoring the engine is the whole job: mods/, the data directory and
           the standalone saves stay where they are.  The loader's own state file
           is cleared first so a later reinstall starts from "nothing enabled",
           which is what the all-in-one installer did. */
        tc::Core core(directory);
        core.apply({});
        tc::atomic(engine, tc::read(backup));
        tc::fs::remove(backup);
        result.changed = true;
        if (phase) phase(SetupPhase::verifying);
        if (tc::hash(tc::read(engine)) != TC_ENGINE_SHA || backup_present(directory)) {
            clear_staging(engine);
            return failure(SetupError::verification_failed,
                           inspection.snapshot.technical_detail +
                               L"恢复后校验失败：引擎与原始备份不一致。\n");
        }
    } catch (const std::exception& error) {
        clear_staging(engine_of(inspection.snapshot.root));
        SetupResult failed = failure(SetupError::io_error, widen(error.what()));
        failed.changed = result.changed;
        return failed;
    }
    result.user_message =
        L"Mod Loader 已卸载，原始引擎已恢复\nMod、配置和独立存档都保留在原处。";
    return result;
}

}  // namespace tc::setup
