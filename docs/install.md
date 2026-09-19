# 安装与维护

## 系统要求

- Windows x64，《Turing Complete》2.1.334 指定构建（哈希见下文）。
- 游戏目录必须可写。玩家侧不需要 Python、Node.js、MinGW 或额外启动器。

## 安装 / 升级

1. 关闭要安装的那份游戏。
2. 双击 `TCModLoader-Setup.exe`。
3. 选择游戏目录中的 `Turing Complete.exe`，在确认框选“是”（安装／升级／修复）。
4. 把 `.mod` 文件直接放入游戏根目录的 `mods/` 文件夹，不要解压。
5. 正常启动游戏，点击主菜单右上角 **Mods**，勾选并“应用更改”。
6. 关闭并重新启动游戏。管理页会区分“下次启动启用”与“本次运行中”。

可从已验证的 0.1.0／0.2.0／0.3.0 安装直接升级。

## 卸载

关闭该份游戏，再运行同版本安装器，选择同一个 `Turing Complete.exe`，在确认框选“否”
（卸载并恢复 Mod 修改的文件）。资源改动与原引擎恢复，Mod 包、备份和插件数据保留。
游戏 EXE 本身从不被修改。

资源文件若被外部工具或游戏更新改动，加载器会拒绝强行覆盖。不要在资源 Mod 启用期间
删除备份目录。

## 独立存档与原版导入

从 0.3.0 起，装有加载器的游戏始终使用独立存档，即使全部 Mod 停用或按住 Shift 跳过
原生插件也一样；首次启动默认是一份新存档。

| 用途 | 位置 |
|---|---|
| 原版存档 | `%USERPROFILE%\AppData\Roaming\Turing Complete` |
| Mod 默认存档 | `%USERPROFILE%\AppData\Roaming\Turing Complete Mods\profiles\default` |
| 导入副本 | 上述 profiles 目录中的 `import-<随机编号>` |

在主菜单 Mods 页面点击“导入原版存档（新副本）”，成功后关闭并重启游戏即可继续原版进度。
页面会显示当前存档路径与待切换副本，也可直接打开当前存档文件夹。

- 导入包含本机关卡进度、设置与电路文件，每次新建副本、逐文件校验；不改原版、
  不合并或覆盖已有 Mod 存档；导入失败不切换存档。
- 运行中的游戏继续使用原副本，重启后才切换。
- Steam 云元数据 `steam_autocloud.vdf` 不复制；这不是云存档下载功能。
- 存档路径在游戏初始化前重定向，只改进程内专用路径常量，磁盘 EXE 不变。此构建实际按
  `USERPROFILE` 拼接路径，所以仅设置 `APPDATA` 不能隔离。
- 当前副本选择保存在 `<游戏目录>/tc-modloader-data/saves.ini`；同 Windows 用户下不同
  游戏安装共享 profiles 目录，但各自的选择独立。

卸载加载器后游戏恢复原版路径，Mod 存档仍保留，绝不自动写回原版；建议同时备份原版与
Mod 存档。原生插件若自行硬编码其他路径，加载器无法阻止它绕开游戏存档接口。

## 启停、更新与恢复

- 原生插件启停与更新需要重启游戏，不支持热卸载。重新打开 Mods 页面会扫描新包，也可
  点击“刷新列表”。
- 原生包应用时记录 SHA-256；包被替换后不会按旧启用状态自动执行新代码，需重新应用并
  重启。
- 依赖先加载；循环依赖被拒绝，依赖失败会阻止依赖它的插件启动。
- 两个 Mod Hook 同一函数时，后加载者失败并在管理页显示状态，不做隐式 Hook 串联。
- 插件初始化失败会撤销它已注册的 Hook，并在日志中说明原因。
- 原生 DLL 与游戏同权限，不在沙箱中；加载器不能可靠隔离原生代码的访问违规或崩溃。
- 启动时按住 Shift 并保持到主菜单出现，可跳过本次所有原生插件，再进入管理页停用问题包。
- 无法进入游戏时，关闭该份游戏后用 `tools/tcmod-cli.exe <游戏目录> disable-all` 保存
  全部停用状态。

日志位于 `<游戏目录>/tc-modloader-data/loader.log`；日志格式与排查方法见
[reference/diagnostics.md](reference/diagnostics.md)。

## 文件位置

| 路径 | 内容 |
|---|---|
| `game_engine.dll` | 加载器代理（原引擎被改名为 `tc_game_engine.dll`） |
| `mods/*.mod` | 玩家分发的包 |
| `tc-modloader-data/state.json` | 部署状态与原生包指纹 |
| `tc-modloader-data/blobs/` | 资源原文件备份 |
| `tc-modloader-data/plugins/<id>/<包指纹>/` | 原生 DLL 缓存 |
| `tc-modloader-data/plugin-data/<id>/` | 插件的持久数据目录 |
| `tc-modloader-data/loader.log` | 运行日志 |

安装包不包含原游戏 EXE 或引擎。原生 DLL 从缓存载入游戏进程，不覆盖游戏自身 DLL。

## 兼容构建

界面版本号 2.1.334，并严格校验以下 SHA-256：

```text
Turing Complete.exe
8875da0e88cc878cb0fce30cfb63d20c5500cae341608af4d786649b88c8eb21

原版 game_engine.dll
0a4030b5f5538cc3682610ac3e7b39e8e31ec34f9aeafe7a1d036bfa33819166
```

游戏更新后需重新适配，不能简单关闭哈希检查。旧 Godot PCK、BepInEx 或 Lua Mod 不能直接
用于本 SDK。可在安装前运行 `tools/compat.ps1 -GameDirectory <游戏目录>` 查看具体画像和文件
证据；画像维护流程见 [compatibility.md](compatibility.md)。

## 排错

| 现象 | 处理 |
|---|---|
| 主菜单没有 Mods 按钮 | 确认加载器已安装到该份游戏，且未同时运行原版启动器 |
| 管理页显示包存在但状态异常 | 查看 `loader.log` 中该包 id 的日志行，按 [diagnostics.md](reference/diagnostics.md) 定位 |
| 装了新包但行为没变 | 需要在管理页重新“应用更改”并重启；包被替换后指纹不匹配会被拒绝 |
| 游戏崩溃或卡在启动 | 关闭游戏，按住 Shift 启动跳到主菜单，进入 Mods 停用问题包后重启 |
