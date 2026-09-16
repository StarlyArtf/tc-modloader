# TC Mod Loader 0.3.0

这是支持修改游戏逻辑的原生代码 Mod 加载器，适用于 Windows x64《Turing Complete》2.1.334 的指定构建。

## 玩家使用

1. 关闭要安装的那份游戏，双击 TCModLoader-Setup.exe。
2. 选择游戏目录中的 Turing Complete.exe，选择“是”安装／升级。
3. 将 .mod 文件直接放入游戏根目录的 mods 文件夹，不要解压。
4. 正常启动游戏，点击主菜单右上角 Mods，勾选并“应用更改”。
5. 关闭并重新启动。页面会区分“下次启动启用”与“本次运行中”。

可从已验证的 0.1.0／0.2.0 安装直接升级。玩家不需要 Python、Node.js、MinGW 或额外启动器。游戏目录必须可写。

## 独立存档与原版导入

从 0.3.0 起，装有加载器的游戏始终使用独立存档，即使全部 Mod 停用或按住 Shift 跳过原生插件也是如此。首次启动默认是一份新存档。

- 原版位置：%USERPROFILE%\AppData\Roaming\Turing Complete
- Mod 默认位置：%USERPROFILE%\AppData\Roaming\Turing Complete Mods\profiles\default
- 导入副本位置：上述 profiles 目录中的 import-随机编号

在主菜单 Mods 页面点击“导入原版存档（新副本）”，成功后关闭并重启游戏，即可继续原版进度。页面会显示当前存档路径和待切换副本，也能打开当前存档文件夹。

导入前请关闭原版游戏。导入包含本机关卡进度、设置和电路文件，每次新建副本，逐文件校验；不修改原版、不合并或覆盖已有 Mod 存档。导入失败不切换存档。运行中的游戏继续使用原副本，重启才切换。Steam 云元数据 steam_autocloud.vdf 不复制；这里不是云存档下载功能，新目录也没有配置原版的云同步。

存档路径在游戏初始化前重定向；只改变进程内专用路径常量，磁盘 EXE 不修改。此构建实际根据 USERPROFILE 拼接路径，不能仅靠 APPDATA 环境变量隔离。

当前副本选择保存在游戏目录 tc-modloader-data/saves.ini；所有已导入副本保留，可手动备份。相同 Windows 用户下，不同游戏安装共享上述 profiles 目录，各安装的选择独立。默认副本为共享的 default。

卸载加载器后，游戏重新使用原版位置，Mod 存档仍保留，绝不自动写回原版。需要同时备份原版及 Mod 存档。原生插件若自行硬编码其他路径，加载器不能阻止它绕开游戏存档接口。

## 实际改变逻辑的示例

example.cycle-guard.mod 是“周期运行守卫”，包含独立的 C++ DLL：

- Hook 游戏的 sim_do 函数，在命令到达仿真线程前修改目标周期。
- 把连续／长运行限制为“当前周期 + N”，可选 1、10、100、1000。
- 保留暂停、重置以及不超过上限的单步／短运行命令。
- 添加调试面板，显示实际周期、拦截次数和修改前后目标，提供批量运行按钮。F6 显示／隐藏面板。

进入已有电路后，先使用一次游戏原有的运行／单步按钮，让插件捕获该电路的仿真上下文。然后选择 100，点击“请求连续运行（验证拦截）”，仿真应在最多再执行 100 个周期后停下；关卡本身也可能提前停止。上下文尚未捕获时，面板的运行按钮为灰色。取消面板中的“拦截游戏运行命令”，后续命令恢复原行为。

示例只拦截经过 sim_do 的运行命令，不保证拦截所有编译／加载路径或其他 Mod 直接访问内核的行为。它不改电路文件。源码位于 examples/cycle-guard/plugin.cpp。

example.menu-demo.mod 是保留的旧版资源示例。

## 开发能力

SDK 定义：sdk/tc_mod_api.h。开发指南：SDK-GUIDE.md。

原生 Mod 可以解析 EXE 中的符号、调用已知签名的游戏函数、安装函数 Hook 并调用原函数、执行每帧逻辑、处理快捷键、使用游戏自身 ImGui 创建新面板。

这是通用底层扩展能力。`sdk/tc_mod.h` 提供聚合入口，覆盖：
- 原型／组件对象模型：内置枚举、模板复制、名称/描述/SVG、自定义元件注册。
- board 选择状态：当前/上一帧选中元件与导线，含计数和 ID 枚举。
- 游戏状态：战役、关卡、字宽、当前输入/输出。
- 仿真：周期、设置、run/pause/reset。
- 导线：读取、取色、添加、放置、更新。
- 存档：保存次数和当前 level/schematic 路径。

文件解析型 `add_custom_prototype` 仍需继续核实完整栈参数布局；调用未核实签名前不能只凭函数名猜测。

## 启停、更新和恢复

原生插件启停和更新需要重启，不做热卸载。重新打开 Mods 页面会扫描新包，也可点击“刷新列表”。

原生包应用时记录 SHA-256。包被替换后，不会按旧启用状态自动执行新代码，需重新应用并重启。依赖先加载；循环依赖被拒绝，依赖失败会阻止依赖它的插件启动。

两个 Mod Hook 同一函数时，后加载者会失败并显示状态，不做隐式 Hook 串联。初始化失败会撤销该插件已注册的 Hook。

原生 DLL 与游戏具有相同权限，不在沙箱中。加载器不能可靠隔离原生代码的访问违规或崩溃。启动时按住 Shift 并保持到主菜单出现，可以跳过本次原生插件，再进入 Mods 停用问题包。

无法进入游戏时，关闭该份游戏后可用 tools/tcmod-cli.exe 游戏目录 disable-all 保存全部停用状态。日志位于 tc-modloader-data/loader.log。

## 卸载

关闭该份游戏，再运行同版本安装器，选择同一个 EXE，在确认框中选择“否”。资源改动及原引擎恢复，Mod 包、备份和插件数据保留。原 EXE 不被修改。

资源文件如果被外部工具或游戏更新修改，加载器会拒绝强行覆盖。不要在资源 Mod 启用期间删除备份目录。

## 兼容构建

界面版本号 2.1.334，同时严格校验以下 SHA-256：

Turing Complete.exe：
8875da0e88cc878cb0fce30cfb63d20c5500cae341608af4d786649b88c8eb21

原版 game_engine.dll：
0a4030b5f5538cc3682610ac3e7b39e8e31ec34f9aeafe7a1d036bfa33819166

游戏更新后需重新适配，不能简单关闭哈希检查。旧 Godot PCK、BepInEx 或 Lua Mod 不能直接用于此 SDK。

## 文件位置

- game_engine.dll：加载器代理。
- tc_game_engine.dll：从玩家本机保留的原版引擎。
- mods/*.mod：玩家分发的包。
- tc-modloader-data/state.json：部署状态及原生包指纹。
- tc-modloader-data/blobs/：资源原文件备份。
- tc-modloader-data/plugins/<id>/<包指纹>/：原生 DLL 缓存。
- tc-modloader-data/plugin-data/<id>/：插件自己的持久数据。

安装包不包含原游戏 EXE 或引擎。原生 DLL 从缓存载入游戏进程，不覆盖游戏自己的 DLL。

## 构建及测试

Windows x64 + MinGW-w64，默认 C:\msys64\ucrt64\bin，可通过 TC_MINGW_BIN 指定。

修改和构建加载器时，先解压分发包中的 TCModLoader-0.3.0-source.zip；完整示例源码也在该源码包中。

依次执行 build.ps1、node tests/run.js、tests/native.ps1、tests/saves.ps1、tests/setup.ps1、package.ps1。

依赖源码 miniz 3.0.2、nlohmann/json 3.11.3、MinHook 1.3.4 已固定并随源码提供，不需联网构建。Node.js 只用于开发测试及导出表维护。

实机和自动测试记录见 VALIDATION.md。项目源码 MIT，第三方许可随包提供。

