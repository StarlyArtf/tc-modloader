#include "setup_state.hpp"

#include "compat.hpp"
#include "version.hpp"

namespace tc::setup {
namespace {

bool destructive_allowed(const InstallationSnapshot& snapshot) {
    return snapshot.loader_state != LoaderState::backup_missing &&
           snapshot.loader_state != LoaderState::backup_invalid;
}

/* The generated compatibility header carries the game version as a narrow
   literal; the UI is wide, and the value is pure ASCII ("2.1.334"). */
std::wstring ascii_wide(const char* text) {
    std::wstring value;
    for (; *text; ++text) value.push_back(static_cast<wchar_t>(*text));
    return value;
}

}  // namespace

std::wstring game_version_text() { return ascii_wide(TC_GAME_VERSION); }

std::wstring result_title(ResultKind kind) {
    switch (kind) {
        case ResultKind::none: return {};
        case ResultKind::installed:
            return L"Mod Loader " TC_MODLOADER_VERSION_WTEXT L" 已安装";
        case ResultKind::upgraded:
            return L"已升级到 Mod Loader " TC_MODLOADER_VERSION_WTEXT;
        case ResultKind::repaired: return L"安装已修复";
        case ResultKind::uninstalled: return L"Mod Loader 已卸载";
        case ResultKind::nothing_changed: return L"未进行任何更改";
        case ResultKind::failed: return L"操作未完成";
    }
    return {};
}

bool result_is_success(ResultKind kind) {
    switch (kind) {
        case ResultKind::installed:
        case ResultKind::upgraded:
        case ResultKind::repaired:
        case ResultKind::uninstalled: return true;
        default: return false;
    }
}

std::wstring compatibility_text(const InstallationSnapshot& snapshot) {
    switch (snapshot.compatibility) {
        case Compatibility::compatible: return L"兼容";
        case Compatibility::unsupported_exe: return L"版本不支持";
        case Compatibility::unsupported_engine: return L"引擎不匹配";
        case Compatibility::unreadable: return L"无法检查";
        case Compatibility::checking: break;
    }
    return L"正在检查…";
}

std::wstring loader_text(const InstallationSnapshot& snapshot) {
    switch (snapshot.loader_state) {
        case LoaderState::not_installed: return L"尚未安装";
        case LoaderState::installed_current:
            return L"已安装 " TC_MODLOADER_VERSION_WTEXT;
        case LoaderState::installed_previous: return L"已安装旧版本";
        case LoaderState::installed_unknown: return L"未知引擎";
        case LoaderState::backup_missing: return L"缺少原始引擎备份";
        case LoaderState::backup_invalid: return L"原始引擎备份损坏";
    }
    return L"未知引擎";
}

std::vector<std::wstring> safety_lines(const InstallationSnapshot& snapshot) {
    std::vector<std::wstring> lines;
    switch (snapshot.compatibility) {
        case Compatibility::compatible:
            lines.push_back(L"游戏版本 " + ascii_wide(TC_GAME_VERSION) +
                            L"：与当前兼容配置一致");
            break;
        case Compatibility::unsupported_exe:
            lines.push_back(L"Turing Complete.exe 不是受支持的构建");
            lines.push_back(L"安装器只会读取，不会改动这份游戏");
            return lines;
        case Compatibility::unsupported_engine:
            lines.push_back(L"game_engine.dll 已被其他程序修改");
            lines.push_back(L"安装器不会覆盖不认识的引擎");
            return lines;
        case Compatibility::unreadable:
            lines.push_back(L"无法读取游戏文件");
            lines.push_back(L"请确认目录完整、磁盘可访问");
            return lines;
        case Compatibility::checking:
            lines.push_back(L"正在读取游戏文件…");
            return lines;
    }
    switch (snapshot.loader_state) {
        case LoaderState::not_installed:
            lines.push_back(L"原始引擎可安全备份");
            break;
        case LoaderState::installed_current:
            lines.push_back(L"原始引擎备份有效");
            break;
        case LoaderState::installed_previous:
            lines.push_back(L"检测到可安全升级的已知旧版本");
            break;
        case LoaderState::installed_unknown:
            lines.push_back(L"无法识别当前引擎");
            break;
        case LoaderState::backup_missing:
            lines.push_back(L"找不到原始引擎备份，已禁止覆盖");
            break;
        case LoaderState::backup_invalid:
            lines.push_back(L"原始引擎备份校验失败，已禁止覆盖");
            break;
    }
    if (snapshot.game_running)
        lines.push_back(L"游戏进程：正在运行，请先关闭");
    else
        lines.push_back(L"游戏进程：未运行");
    if (!snapshot.directory_writable) lines.push_back(L"目录不可写：需要管理员权限");
    return lines;
}

std::vector<LineTone> safety_tones(const InstallationSnapshot& snapshot) {
    std::vector<LineTone> tones;
    switch (snapshot.compatibility) {
        case Compatibility::compatible:
            tones.push_back(LineTone::good);
            break;
        case Compatibility::checking:
            tones.push_back(LineTone::normal);
            break;
        default:
            tones.push_back(LineTone::warn);
            tones.push_back(LineTone::warn);
            return tones;
    }
    switch (snapshot.loader_state) {
        case LoaderState::not_installed:
        case LoaderState::installed_current:
        case LoaderState::installed_previous:
            tones.push_back(LineTone::good);
            break;
        default:
            tones.push_back(LineTone::warn);
            break;
    }
    if (snapshot.game_running || !snapshot.directory_writable)
        tones.push_back(LineTone::warn);
    else
        tones.push_back(LineTone::normal);
    return tones;
}

ActionSet actions_for(const InstallationSnapshot& snapshot) {
    ActionSet set;
    const std::wstring version = TC_MODLOADER_VERSION_WTEXT;
    const MenuEntry rescan{Action::rescan, L"重新检查", true};
    const MenuEntry directory{Action::open_directory, L"打开游戏目录", true};
    const MenuEntry copy{Action::copy_technical, L"复制技术信息", true};
    const MenuEntry technical{Action::technical, L"查看技术信息", true};

    if (snapshot.compatibility == Compatibility::checking) {
        set.primary_label = L"正在检查…";
        set.secondary = Action::none;
        set.menu = {rescan};
        return set;
    }
    if (snapshot.compatibility != Compatibility::compatible) {
        set.primary_label = L"无法安装到此位置";
        set.secondary = Action::technical;
        set.secondary_label = L"查看技术信息";
        set.menu = {rescan, {Action::choose_location, L"选择其他位置", true}, directory,
                    copy};
        return set;
    }
    if (snapshot.game_running) {
        set.primary = Action::retry;
        set.primary_label = L"关闭游戏后重试";
        set.primary_enabled = true;
        set.secondary = Action::open_directory;
        set.secondary_label = L"打开游戏目录";
        set.menu = {rescan, directory, technical, copy};
        return set;
    }
    if (!destructive_allowed(snapshot)) {
        set.primary_label = L"已禁用安装";
        set.secondary = Action::technical;
        set.secondary_label = L"查看技术信息";
        set.menu = {directory, {Action::choose_location, L"选择其他位置", true}, rescan,
                    copy};
        return set;
    }
    if (!snapshot.directory_writable) {
        set.primary = Action::elevate;
        set.primary_label = L"使用管理员权限安装";
        set.primary_enabled = true;
        set.secondary = Action::open_directory;
        set.secondary_label = L"打开游戏目录";
        set.menu = {rescan, technical, copy};
        return set;
    }
    const MenuEntry uninstall{Action::uninstall, L"卸载并恢复原始引擎…", true};
    const MenuEntry mods{Action::open_mods, L"打开 Mods 文件夹", true};
    const MenuEntry log{Action::open_log, L"查看安装日志", true};
    switch (snapshot.loader_state) {
        case LoaderState::not_installed:
            set.primary = Action::install;
            set.primary_label = L"安装到此位置";
            set.primary_enabled = true;
            set.secondary = Action::open_directory;
            set.secondary_label = L"打开游戏目录";
            set.menu = {rescan, technical, copy};
            return set;
        case LoaderState::installed_current:
            set.primary = Action::repair;
            set.primary_label = L"修复安装";
            set.primary_enabled = true;
            set.secondary = Action::open_mods;
            set.secondary_label = L"打开 Mods 文件夹";
            set.menu = {uninstall, log, directory, rescan, technical, copy};
            return set;
        case LoaderState::installed_previous:
            set.primary = Action::upgrade;
            set.primary_label = L"升级到 " + version;
            set.primary_enabled = true;
            set.secondary = Action::open_log;
            set.secondary_label = L"查看安装日志";
            set.menu = {uninstall, mods, directory, rescan, technical, copy};
            return set;
        default:
            break;
    }
    /* Compatible game, but the engine is neither the original nor a known
       loader: read-only, exactly like the missing-backup states. */
    set.primary_label = L"已禁用安装";
    set.secondary = Action::technical;
    set.secondary_label = L"查看技术信息";
    set.menu = {directory, {Action::choose_location, L"选择其他位置", true}, rescan, copy};
    return set;
}

}  // namespace tc::setup
