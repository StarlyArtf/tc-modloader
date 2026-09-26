# 下一步交接：从"能读"到"能改"

更新时间：2026-09-20

> **接手结论**：读的那一半基本齐了（棋盘、元件、引脚、连接、信号值、事件）；下面六件事按
> "能解锁多少 Mod ÷ 成本"排序，前三件建议连着做——一件是几行的修正，一件是反汇编核实，
> 一件是把**已经有**的字段服务化。
>
> 计划表里的三项（撤销分组、让游戏选中、棋盘高亮/标注/镜头）**不在本文的动手范围**，
> 那些等明确指令，见 [PLAN-backlog.md](PLAN-backlog.md)。

## 2026-09-20 本轮进展

- 第 1 项已完成：`TC_EVENT_LEVEL_LOAD` 现在传钩子里的真实名称；离线事件用例收到
  `offline_level`，真机两次收到 `sandbox`，playtest 明确拒绝 `(none)`。
- 第 2 项完成了第一层核实：`board_delete_component(board+0x78,index)` 真机能删除目标活记录，但
  会留下 kind 0 墓碑；放置会复用它。这个低层入口不自行写撤销历史，尚未进命令总线。反汇编找到
  撤销感知的 `try_delete(board, component_indices, wire_ids)`，下一步先测 delete → undo → redo。
- 移动仍未找到独立入口：`board_commit_move` 只是提交已经发生的拖拽，依赖选择集与拖拽状态，
  没有目标坐标参数。选择相关符号只记录、没有动手，遵守计划边界。
- 第 3 项（关卡信息只读探针）尚未开始，仍是下一轮首项。

## 先分清"为什么没有"，再决定怎么做

| 类别 | 意思 | 本文里的项目 |
|---|---|---|
| **A 只是没做** | 没有技术障碍，排期问题 | 1（关卡名）、3（关卡信息服务化）、4（Mod 自己的数据）、5（调度器与通信） |
| **B 有线索待核实** | `nm` 已看到候选符号，差"看参数 + 真机试行为" | 2（删除/移动元件） |
| **C 已知要大工程** | 要先做研究交接再决定 | 6（逐周期、编译层） |

## 1. 修好"关卡加载事件"里的关卡名（A 类，几行）

**想要什么**：进关卡时，事件里真的带着关卡名。

**现状（这是个契约与实现不一致的小问题）**：`sdk/tc_event_api.h` 里 `TC_EVENT_LEVEL_LOAD` 的注释
写着 `name: level name`，但加载器派发时传的是空——真机日志就是
`PROBE: level.load name=(none)`。而名字其实就在手边：`TCHookLevelLoadArgs.name`。

**第一步**：`src/native.hpp` 里 level.load 的派发处把 `args->name`（Nim 字符串）转成 C 字符串传
进去。仓库里已有同样的转换写法（`tc_save_model.h` 的 `nimStringCStr`，即 `data + 8`）。

**验收**：真机 `game-handle-probe` 日志里那行从 `name=(none)` 变成实际名字，playtest 加一条断言；
fast / host / 该真机用例全绿。

**注意**：这个字符串只在回调期间有效（借用），文档要写明"不要存起来"。

**如果不成**：若某些路径下 `args->name` 不是关卡名，就退一步只承诺"非空时给出名字"，并把限制写进
文档，而不是猜。

## 2. 核实"删除 / 移动元件"的入口（B 类）

**想要什么**：Mod 能删掉或移动棋盘上的元件。这是**所有工具类 Mod 的门槛**——现在放上去就撤不回来，
只能让玩家自己按撤销。

**现状**：添加那条链已经核实过（`add_component__presenterZutilitiesZhelper95functions_u5918`
只需 board model）；删除/移动还没有入口。

**第一步**：

```powershell
# 候选筛选（读-only，随便跑）
nm "Turing Complete.exe" | Select-String "modelZboardZboard" |
  Select-String "del|remove|delete|erase|move|update"
```

挑最像的 1–2 个做反汇编（`objdump -d`）看参数布局与返回值，然后写一次性真机探针：
**放一个元件 → 调候选 → 用公开路径回读**（枚举 + 读数）确认元件数少了一个、那个坐标查不到。

**只有真机验证过的才进命令总线**，做法照抄 `PLACE_COMPONENT`：
新命令类型 → 入队校验 → 执行 → 真机断言 → 文档。

**附带产出**：同一批候选里就有 `select_component` 等"设置选中"的符号。如果顺手看到了它的签名，
**只报告，不要动手**——那属于计划表。

## 3. 关卡信息服务化（A 类，工作量中等）

**想要什么**：Mod 能问"现在在哪一关、是不是战役、这关几个输入输出、完成没有"。

**现状（比想象的多，但都是裸指针）**：

- 已经能读：是不是战役、战役名、当前字长、数字显示方式、是否允许手动控制周期；
- **已经标注不可信**：`level_used_input` / `level_used_outputs`（波形模块因此改成"数棋盘上的
  输入输出元件"）；
- 完全没有：关卡名（见第 1 件）、编号/路径、完成状态、进度写入、关卡列表、导航命令。

**第一步**：写**只读探针**，在"主菜单 / 刚进关卡 / 换了关卡 / 跑完一关"四种时刻各打一遍这些字段，
逐个标注"可信 / 不可信"；再决定做成独立的 `tc.level` 服务，还是并进 `tc.board`。

**验收**：服务化之后，真机探针打印"关卡名 + 是否战役 + 输入/输出数"，并且输入/输出数与**棋盘上的
输入输出元件数**一致——这是目前唯一信得过的交叉验证。

**风险**：这类全局量可能在某些关卡类型下不写。宁可给"不可用"标记，也不要猜一个值。

## 4. Mod 自己的配置与数据（A 类，生态刚需）

**想要什么**：Mod 有自己的设置，以及"跟着存档走"的数据——而不是自己写文件、换存档就丢。

**现状**：只有每个 Mod 的目录（`plugin-data/<id>/`）；"存一次档"能触发，但 Mod 的数据存不进去。

**第一步（拆两小步，别一起做）**：

1. **每 Mod 的键值配置**：纯加载器侧，读写一个按 Mod id 隔离的配置，没有技术障碍；
2. **跟关卡/存档绑定的数据**：先搞清楚存档目录结构与"提交成功后再保存"的时序，保证"存了能读回来"。

**验收**：配置部分——写完 → 重启游戏 → 读回相同值；关卡数据部分——存一次 → 换关 → 回关 → 数据还在。

**注意**：这是"没做"里设计量最大的一项，动手前建议先写半页设计（键/值类型、隔离边界、失败时怎么办）。

## 5. 调度器 + Mod 间通信（A 类，成本低）

**想要什么**：① Mod 能说"3 帧后 / 第 100 周期 / 关卡加载后做这件事"，不用自己数帧；② 两个 Mod 能
互相找到并调用，而不是只靠事件广播。

**第一步**：调度器先做最小版（按帧、按周期两种触发 + 取消），因为它是纯加载器侧；Mod 间通信先做
"按 id 查对方在不在 + 简单请求/应答"，不要一上来就做消息总线。

**验收**：离线单测覆盖触发次序、取消、回调抛异常的隔离；再加一个真机小用例（关卡加载后触发一次）。

## 6. 两块大工程（C 类，先别动手）

| 项目 | 为什么大 | 建议的第一步 |
|---|---|---|
| 逐周期采样/回调 | 这个构建里没有可挂钩子的"每周期步进"函数（循环在生成的机器码里），只能往生成的代码里插探针 | 先出一份**研究交接**：能注入什么、注入点在哪、缓冲与线程契约怎么定 |
| 编译层观测（展开、代价、时序） | 需要新的观测服务 + 反推 | 同上，先研究交接 |

## 更靠后、取决于你想要什么的

- 棋盘上的绘制与镜头（计划表里那条）—— 先摸清相机状态（缩放/平移存在哪）；
- 棋盘上的点击/拖拽事件、合成输入；
- 诊断细节（"为什么这个服务不可用"、配额还剩多少）；
- 把玩家画的电路导出成文件（导入已经有了）。

## 接手第一组命令

```powershell
cd D:\p\tc-modloader

powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier host -NoBuild -KeepGoing
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier game -Name game-handle-probe -NoBuild -KeepGoing
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Name component-placement -NoBuild -KeepGoing
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi.ps1
```

## 干活的规矩（本项目一直这么做）

1. **证据优先**：先写只读探针把真实值打出来 → 再定字段 → 再写真机断言 → **最后**才进契约。
   真机推翻过我们两次（导线端点的读法、撤销的墓碑），所以"先报数值再定断言"不是形式主义。
2. **进契约要成套**：SDK 头 + `tools/abi-snapshot.cpp`（再跑 `tools/abi.ps1 -Update`）+ `docs/sdk/*`
   + `changelog.md`；涉及行为就补 `verification.md`，涉及上限就补 `reference/limits.md`。
3. **新增离线单测要同时改 `build.ps1`**，否则没有任何一层会跑到它。
4. **写文件用 LF**：`Set-Content` 会写成 CRLF，本项目要求 LF。
5. **计划表里的三项没有明确指令不要动**（撤销分组、让游戏选中、棋盘高亮/标注/镜头）。

## 相关文档

| 文档 | 内容 |
|---|---|
| [HANDOFF-game-services.md](HANDOFF-game-services.md) | 现有什么、真机证据、10 条风险、踩坑清单 |
| [PLAN-backlog.md](PLAN-backlog.md) | 已确认想要、等你下令的项目 |
| [research/board-object-fields.md](research/board-object-fields.md) | 字段与引脚语义的收口过程，以及选择集写入的候选符号 |
| [sdk/services.md](sdk/services.md) | 服务表逐版说明（V1–V6 + simulation） |
| [verification.md](verification.md) | 每一块能力是怎么验的、证据长什么样 |
