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
| 命令总线：排队修改、状态与错误 | [sdk/commands.md](sdk/commands.md) |
| 游戏生命周期与结构/选择变化事件 | [sdk/lifecycle.md](sdk/lifecycle.md) |
| 事务预检、原生撤销/重做与保存屏障 | [sdk/transactions.md](sdk/transactions.md) |
| 宿主 API、Hook、线程与生命周期 | [sdk/host-api.md](sdk/host-api.md) |
| 版本化游戏服务与 Board 句柄 | [sdk/services.md](sdk/services.md) |
| 元件原型、电路导入、内置表、代价与延迟 | [sdk/game-model.md](sdk/game-model.md) |
| 原生逻辑回调（形状、位宽、相位） | [sdk/custom-logic.md](sdk/custom-logic.md) |
| 插件界面（面板、控件、视口、热键） | [sdk/ui.md](sdk/ui.md) |
| 仿真、board、导线、存档、游戏状态 | [sdk/simulation.md](sdk/simulation.md) |
| 上限与约束速查 | [reference/limits.md](reference/limits.md) |
| 加载器能力（插件能拿到什么、包怎么声明） | [reference/capabilities.md](reference/capabilities.md) |
| 符号别名与钩子链（多个 Mod 共用同一个游戏函数） | [reference/symbols.md](reference/symbols.md) |
| 日志与故障排查 | [reference/diagnostics.md](reference/diagnostics.md) |
| 未使用接口登记（已交付、当前没人用、怎么接回来） | [reference/unused-interfaces.md](reference/unused-interfaces.md) |
| 验证体系、场景清单、复现命令 | [verification.md](verification.md) |
| 游戏兼容性画像、离线识别与 SDK ABI 基线 | [compatibility.md](compatibility.md) |
| 维护者发布加载器、确定性打包、发布清单 | [releasing.md](releasing.md) |
| 版本历史 | [changelog.md](changelog.md) |
| 接手：服务与编辑地基的现状、证据、未决项 | [HANDOFF-game-services.md](HANDOFF-game-services.md) |
| 接手：打孔纸带、宽位常量即时更新与真机验收 | [HANDOFF-punch-tape-runtime-constants.md](HANDOFF-punch-tape-runtime-constants.md) |
| 接手：左侧 IO 面板的引脚顺序（`tc.pin_order` + `local.pin-order`） | [HANDOFF-pin-order.md](HANDOFF-pin-order.md) |
| 接手：元件接口（阶段 1 前半：类型目录 `tc.component.registry`） | [HANDOFF-component-registry.md](HANDOFF-component-registry.md) |
| 接手：元件接口 M5（geometry 第一刀 + render V1 覆盖层 + V2 关闭默认绘制） | [HANDOFF-component-m5.md](HANDOFF-component-m5.md) |
| 接手：选中提示（白弧）跟随 footprint（2026-09-23 已收口：宿主按类型关掉游戏那圈） | [HANDOFF-selection-hint.md](HANDOFF-selection-hint.md) |
| 接手：元件接口 V2（阶段 1 收口：脚数 >8 与值模型） | [HANDOFF-component-v2.md](HANDOFF-component-v2.md) |
| 接手：元件接口 M2（实例、状态与生命周期） | [HANDOFF-component-m2.md](HANDOFF-component-m2.md) |
| 接手：元件接口 M3 第一刀（配置/仿真状态与内存快照） | [HANDOFF-component-m3.md](HANDOFF-component-m3.md) |
| 研究：0 脚元件（纯源/纯汇）的导入与调度实测 | [research/custom-component-pins.md](research/custom-component-pins.md) |
| 研究：原版内存编辑界面（十六进制编辑器、打孔卡、重置数据与只读门） | [research/memory-editor.md](research/memory-editor.md) |
| 计划表：已确认想要、等你下令才动手的项目 | [PLAN-backlog.md](PLAN-backlog.md) |
| 目标计划：完整自定义元件、Keyboard 与 Screen 外设接口 | [PLAN-custom-components.md](PLAN-custom-components.md) |
| 浮点元件族方案（M0 兼容性与 M1 数值内核已完成 2026-09-25，待做 M2 垂直切片） | [PLAN-float-components.md](PLAN-float-components.md) |
| 计划：沙盒独立仿真器（对标真实数字电路仿真器，方案） | [PLAN-sandbox-simulator.md](PLAN-sandbox-simulator.md) |
| 归档：旧门级延迟源码改写（未使用） | [../unused/gate-delay/README.md](../unused/gate-delay/README.md) |
| 下一步交接：按优先级排好的动手清单（从"能读"到"能改"） | [HANDOFF-next-steps.md](HANDOFF-next-steps.md) |

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
    services.md              版本化服务发现与 Board API
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
    unused-interfaces.md     已交付但当前没有使用者的能力与开关（含接回来的步骤）
  verification.md            验证体系与场景清单
  compatibility.md           游戏构建画像与 SDK ABI 快照
  releasing.md               维护者发布流程与确定性产物
  changelog.md               版本历史
  HANDOFF-game-services.md   服务与编辑地基交接（现状、证据、未决项、线索）
 PLAN-backlog.md            待办计划表（等你下令；动手前不动契约）
 PLAN-custom-components.md  完整自定义元件与外设接口目标计划
PLAN-float-components.md   浮点元件族方案（M0/M1 已通过，见 §4.4 与 §5.2b；M2 待做）
  PLAN-sandbox-simulator.md  沙盒独立仿真器方案（网表化 + 事件驱动 + 四态 + 时序弧）
  HANDOFF-next-steps.md      下一步交接（按优先级排好的动手清单）
  HANDOFF-selection-hint.md  选中提示（白弧）跟随 footprint 的交接（已收口；结论与修正见文首）
  research/                  内部研究日志（非发布文档）
    board-object-fields.md   棋盘对象字段与引脚描述符的语义收口
    component-pipeline.md
unused/gate-delay/          已停用的旧源码改写、测试、工具与文档
    waveform-handoff.md      关卡波形面板交接（现状、证据、未完成项）
    toolbar-tool-handoff.md  工具栏插槽/导线调色盘交接（接口、注入点、未解决项）
    word-watchee-64.md       数值标签只显示低 32 位的定位、修复与 >32 位双排十进制显示
    memory-editor.md         原版内存编辑界面（十六进制编辑器、打孔卡、重置数据、只读门）
  ```

## 用词约定

- “游戏”：Windows x64《Turing Complete》2.1.334 的指定构建（哈希见 [install.md](install.md)）。
- “加载器”：TC Mod Loader，安装后以 `game_engine.dll` 代理身份进入游戏进程。
- “原生插件 / 原生 Mod”：含 DLL 的 `format: 2` 包；`format: 1` 是纯资源包。
- “元件定义 / 定义文件”：描述一个电路封装元件的 `circuit.data` 序列化内容。
- 所有“已验证”结论都指在隔离游戏副本 + 独立存档中真机跑过；证据与复现见
  [verification.md](verification.md)。
