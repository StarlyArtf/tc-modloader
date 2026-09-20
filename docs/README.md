# TC Mod Loader 文档

文档按“读者类型 + 文档类型”分层。根目录 `README.md` 是项目入口，本页是文档索引；
`docs/research/` 下是内部研究日志，不属于发布文档。

## 我该看哪一篇

| 你的目标 | 文档 |
|---|---|
| 安装、升级、卸载、存档隔离、兼容性 | [install.md](install.md) |
| 打包或发布一个 Mod（含原生插件） | [mod-format.md](mod-format.md) |
| 随附示例怎么用 | [examples.md](examples.md) |
| 写第一个原生 Mod | [guides/first-native-mod.md](guides/first-native-mod.md) |
| 让自定义元件的行为由 C++ 决定 | [guides/component-with-cpp-logic.md](guides/component-with-cpp-logic.md) |
| SDK 总览、头文件地图、编译打包 | [sdk/README.md](sdk/README.md) |
| 宿主 API、Hook、线程与生命周期 | [sdk/host-api.md](sdk/host-api.md) |
| 元件原型、电路导入、内置表、代价与延迟 | [sdk/game-model.md](sdk/game-model.md) |
| 原生逻辑回调（形状、位宽、相位） | [sdk/custom-logic.md](sdk/custom-logic.md) |
| 插件界面（面板、控件、视口、热键） | [sdk/ui.md](sdk/ui.md) |
| 仿真、board、导线、存档、游戏状态 | [sdk/simulation.md](sdk/simulation.md) |
| 上限与约束速查 | [reference/limits.md](reference/limits.md) |
| 加载器能力（插件能拿到什么、包怎么声明） | [reference/capabilities.md](reference/capabilities.md) |
| 符号别名与钩子链（多个 Mod 共用同一个游戏函数） | [reference/symbols.md](reference/symbols.md) |
| 日志与故障排查 | [reference/diagnostics.md](reference/diagnostics.md) |
| 验证体系、场景清单、复现命令 | [verification.md](verification.md) |
| 游戏兼容性画像、离线识别与 SDK ABI 基线 | [compatibility.md](compatibility.md) |
| 维护者发布加载器、确定性打包、发布清单 | [releasing.md](releasing.md) |
| 版本历史 | [changelog.md](changelog.md) |

## 目录结构

```text
README.md                    项目概览与入口
docs/
  install.md                 使用者：安装与维护
  mod-format.md              作者：包格式规范
  examples.md                随附示例用法
  sdk/                       SDK 参考（每篇独立可查）
    README.md                总览与头文件地图
    host-api.md              宿主 API 与 Hook
    game-model.md            元件/原型/代价
    custom-logic.md          自定义逻辑回调
    ui.md                    插件界面（ImGui 封装）
    simulation.md            仿真/board/存档
  guides/                    按步骤教程
    first-native-mod.md
    component-with-cpp-logic.md
  reference/                 速查表
    limits.md
    capabilities.md          加载器能力、版本约束
    symbols.md               符号别名表与钩子链
    diagnostics.md
  verification.md            验证体系与场景清单
  compatibility.md           游戏构建画像与 SDK ABI 快照
  releasing.md               维护者发布流程与确定性产物
  changelog.md               版本历史
  research/                  内部研究日志（非发布文档）
    component-pipeline.md
    waveform-handoff.md      关卡波形面板交接（现状、证据、未完成项）
    toolbar-tool-handoff.md  工具栏插槽/导线调色盘交接（接口、注入点、未解决项）
```

## 用词约定

- “游戏”：Windows x64《Turing Complete》2.1.334 的指定构建（哈希见 [install.md](install.md)）。
- “加载器”：TC Mod Loader，安装后以 `game_engine.dll` 代理身份进入游戏进程。
- “原生插件 / 原生 Mod”：含 DLL 的 `format: 2` 包；`format: 1` 是纯资源包。
- “元件定义 / 定义文件”：描述一个电路封装元件的 `circuit.data` 序列化内容。
- 所有“已验证”结论都指在隔离游戏副本 + 独立存档中真机跑过；证据与复现见
  [verification.md](verification.md)。
