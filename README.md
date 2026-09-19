# TC Mod Loader

《Turing Complete》2.1.334（Windows x64 指定构建）的原生代码 Mod 加载器。
它在不修改游戏 EXE 的前提下，把加载器代理成 `game_engine.dll` 进入进程，让 Mod 可以：

| 能力 | 说明 | 文档 |
|---|---|---|
| 资源 Mod | 替换/新增游戏资源文件、精确文本补丁、停用恢复 | [docs/mod-format.md](docs/mod-format.md) |
| 原生插件 | 解析游戏符号、Hook 函数、每帧回调、用游戏自身 ImGui 画面板 | [docs/sdk/host-api.md](docs/sdk/host-api.md) |
| 插件界面 | `sdk/tc_ui.h`：面板/窗口作用域、类型化控件、视口尺寸、热键；导出在加载期统一校验 | [docs/sdk/ui.md](docs/sdk/ui.md) |
| 自定义绘图 | `sdk/tc_ui_draw.h`：交互画布、线／圆／多边形／曲线／文本、局部坐标与嵌套裁剪 | [docs/sdk/ui.md](docs/sdk/ui.md) |
| 图片纹理 | `sdk/tc_ui_texture.h`：图片／RGBA 上传、UV 裁切与翻转、图片按钮、按 Mod 管理与延迟释放 | [docs/sdk/ui.md](docs/sdk/ui.md) |
| 电路封装元件 | 导入 `circuit.data` 生成自定义元件，菜单放置、保存重载 | [docs/sdk/game-model.md](docs/sdk/game-model.md) |
| 声明式 C++ 元件 | 只声明引脚和回调，自动生成定义，无需手写电路文件 | [docs/sdk/native-components.md](docs/sdk/native-components.md) |
| 声明延迟 | 元件声明的关键路径进入编译期时序统计 | [docs/sdk/game-model.md](docs/sdk/game-model.md#设计代价与声明延迟) |
| 原生逻辑回调 | 元件每周期行为由插件 C++ 决定，关卡判定/暂停/重置仍由游戏执行 | [docs/sdk/custom-logic.md](docs/sdk/custom-logic.md) |
| 独立存档 | 始终使用独立存档，可导入原版存档副本 | [docs/install.md](docs/install.md) |
| 能力协商 | 插件按 `TC_CAP_*` 位判断宿主支持什么；包在 `mod.json` 里声明所需能力与依赖版本 | [docs/reference/capabilities.md](docs/reference/capabilities.md) |
| 符号别名与钩子链 | 插件用 `sim.do` 这类稳定别名取地址；多个 Mod 可以加入同一个游戏函数的钩子链 | [docs/reference/symbols.md](docs/reference/symbols.md) |
| 事件总线 | 订阅关卡加载、场景切换、仿真命令、保存等事件，不必自己钩内部函数 | [docs/reference/capabilities.md](docs/reference/capabilities.md) |

版本号只有一个来源：仓库根目录的 `VERSION`；`build.ps1` 由它生成 `src/version.hpp`，
加载器横幅、安装器标题、`tcmod-cli --version` 与分发包文件名都读同一处，不再各写各的。
源码树为 **0.6.0**：在 0.4.0 分发包之后加入了声明式元件、原生逻辑形状/位宽扩展、
插件界面页面与插槽、纹理、波形导出，以及能力协商与依赖版本约束
（见 [docs/changelog.md](docs/changelog.md)）；尚未重新打分发压缩包。

## 玩家：三步使用

1. 关闭游戏，运行 `TCModLoader-Setup.exe`，选择该份游戏的 `Turing Complete.exe`。
2. 把 `.mod` 文件放进 `<游戏目录>/mods/`（不要解压）。
3. 启动游戏 → 主菜单右上角 **Mods** → 勾选 → 应用更改 → 重启。

安装、升级、卸载、存档隔离与排错见 **[docs/install.md](docs/install.md)**。

## 开发者：从哪里开始

| 目标 | 入口 |
|---|---|
| 写第一个原生 Mod | [docs/guides/first-native-mod.md](docs/guides/first-native-mod.md) |
| 让自定义元件由 C++ 决定行为 | [docs/guides/component-with-cpp-logic.md](docs/guides/component-with-cpp-logic.md) |
| SDK 头文件地图与编译打包 | [docs/sdk/README.md](docs/sdk/README.md) |
| 上限与约束速查 | [docs/reference/limits.md](docs/reference/limits.md) |
| 日志与故障排查 | [docs/reference/diagnostics.md](docs/reference/diagnostics.md) |
| 全部文档索引 | [docs/README.md](docs/README.md) |

SDK 定义在 `sdk/`：`tc_mod_api.h`（宿主入口）、`tc_mod.h`（聚合模型）、
`tc_game_model.h`、`tc_component_model.h`、`tc_board_model.h`、`tc_game_state.h`、
`tc_simulation.h`、`tc_wire_model.h`、`tc_save_model.h`、`tc_logic_api.h`（原生逻辑回调）、
`tc_hook.h` / `tc_hook_api.h`（钩子链）、`tc_trace.h`（关卡波形与 VCD 导出）、
`tc_ui.h`（插件界面）。

## 示例

| 包 | 内容 |
|---|---|
| `example.cycle-guard.mod` | Hook `sim_do`，把连续运行限制为“当前周期 + N”（1/10/100/1000），带调试面板 |
| `example.circuit-and.mod` | 注册两输入一输出 AND 自定义元件，并声明设计统计 `(1 门, 1 延迟)` |
| `example.custom-or.mod` | 元件行为由 C++ 回调决定；真机回归 11 个场景 |
| `example.byte-adder.mod` | 单字节加法器（进位入/出），声明 **1 门 1 延迟**，行为由 C++ 决定 |
| `dev.cost-watch.mod` | 只读诊断：把界面分数、各 kind 代价与原型字段写进日志 |
| `dev.kind-list.mod` | 只读探测：枚举 125 个内置元件的 kind／名称／引脚 |
| `dev.menu-demo.mod` | 主菜单页面注册与控件 ID 隔离示例 |
| `example.drawing-demo.mod` | 自定义绘图页面与 F8 面板，支持拖动曲线控制点、网格和裁剪 |
| `example.board-panel.mod` | 电路板侧栏面板：宿主提供的插槽、真实点击、随关卡关闭 |
| `example.waveform-demo.mod` | 关卡波形面板：按周期画输入／输出方波，可导出 VCD 与图片 |
| `tcmod.mod-inspector.mod` | 只读状态面板：战役、存档、周期、选中元件与导线；视口与鼠标坐标 |

每个示例的用法（含 cycle-guard 面板步骤与对照实验）见
**[docs/examples.md](docs/examples.md)**；源码在 `examples/`。

## 构建与测试

Windows x64 + MinGW-w64（默认 `C:\msys64\ucrt64\bin`，可用 `TC_MINGW_BIN` 覆盖）。
依赖 miniz 3.0.2、nlohmann/json 3.11.3、MinHook 1.3.4 已随源码固定，构建不需要联网。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast  # 构建、单元测试、包管理、存档
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier host  # 原生宿主、钩子链、安装器
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier game  # 构建后串行运行真机回归
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier all   # 全部自动化回归
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -List       # 测试 ID、分层与超时
```

测试目录由 `tests/test-catalog.json` 统一描述；可用 `-Name native-logic` 或
`-Name 'ui-board-panel*'` 只跑指定用例，已有构建产物时加 `-NoBuild`。
每次运行把 JSON、JUnit XML 和逐用例 stdout/stderr 写到 `build/test-results/`。
人工 IME／可见页面测试会列在目录中，但不会被 `-Tier all` 自动启动。

真机用例都会复制游戏到 `build/` 下的隔离副本并切换 `USERPROFILE`/`APPDATA`，不触碰玩家存档。
完整清单与断言语义见 **[docs/verification.md](docs/verification.md)**。

正式发布：`powershell -File tools/release.ps1`。它按 `release/manifest.json` 从空 staging
收集内容，验证所有公开 Mod，生成可复现的源码包／玩家包、`release.json` 和两层
`SHA256SUMS.txt`；工作树不干净时默认拒绝。只查看计划用 `tools/release.ps1 -Plan`，完整说明见
**[docs/releasing.md](docs/releasing.md)**。`package.ps1` 保留为“使用已有构建、跳过测试”的兼容入口。

## 兼容构建

仅支持 2.1.334 指定构建，并严格校验 EXE 与引擎 SHA-256（见
[docs/install.md](docs/install.md#兼容构建)）。可用 `tools/compat.ps1 -GameDirectory <游戏目录>`
离线识别；画像格式、新版本适配流程和 SDK ABI 快照见
[docs/compatibility.md](docs/compatibility.md)。游戏更新后需重新适配。

## 许可

项目源码 MIT；第三方组件许可随分发包提供（`licenses/`）。
