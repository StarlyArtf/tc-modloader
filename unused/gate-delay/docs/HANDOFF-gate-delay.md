# 沙盒门级传播延迟：交接

更新时间：2026-09-24

> **接手结论**：模式本身（逐单位传播、按器件延迟发布、环合法、锁存/振荡、开关与沙盒门控）
> **已经能玩，并在真机上验证过**；玩家直接画出的闭合环会**按精确引脚方向**自动剪开、接回并
> 振荡/锁存——门不再限于 NAND：引脚几何取自游戏自己的原型表，AND/OR/NOR/XOR/NOT/Mux/加法器…
> 都在内（§5.7、计划 §28）；自定义实例、文本框等留在同一块板上也不阻止剪环（§5.1、§5.5、§26）。
> 剪不了的只有两类：环穿过**自定义元件内部**，或环上找不到"单输出生产者"的边（§5.7 末尾）。
> 另外，前两次"无响应"其实是**崩溃**，根因是环检测钩子漏转发参数，已修（§5.6、计划 §27）——
> 现在可剪的环走延迟模型，剪不了的环得到游戏原本的 `flag=1`，两者都不再打死游戏。
> 2026-09-24 的 CPU 沙盒编译风暴与修复见 §5.2、计划 §24。计划与全部证据在
> [PLAN-gate-delay.md](PLAN-gate-delay.md)（§13–§26），本文只讲"要接手什么、怎么验、别踩什么"。

## 1. 这是什么

沙盒里的每个元件按**自己的延迟**占若干个"单位"，一个周期 = K 个单位：每个单位先提交
到期结果、再快照状态，各元件按快照求值并按 `d_i` 发布。于是信号逐级传播、交叉耦合环能锁存、
奇数反相环会振荡、"时钟太快"能采到未稳定的值。**只在沙盒生效**，开关在游戏自己的设置页，
默认关闭时与未装 Mod 逐字节一致。

## 2. 落点（代码地图）

| 文件 | 内容 |
|---|---|
| [src/gate_delay.hpp](../src/gate_delay.hpp) | 核心：运行时（提交队列/快照/脏标记）+ 源码改写器（两个 cycle body、LATE 与宿主守卫、接回） |
| [src/native.hpp](../src/native.hpp) | 开关与门控：设置页 checkbox、`level.load` 记关卡名与 board model、剪开钩子（preorder 入口 + 两个环检测函数）、存档前还原 |
| [src/native_logic.hpp](../src/native_logic.hpp) | 宿主函数注册（`tc_delay_begin/read/write/reset/dirty`）、`compile()` 钩子里的改写、gated 转储 |
| [src/component_timing.hpp](../src/component_timing.hpp) | 已有的 `preorder` 时序 shim；本文只加了 `beforeGraph/afterGraph` 回调 |
| [src/gate_pins.generated.hpp](../src/gate_pins.generated.hpp) + [tools/generate-gate-pins.py](../tools/generate-gate-pins.py) | 自动剪环用的引脚表：由游戏自己的原型表（`build/kinds.txt`）生成，只收组合逻辑原型 |
| [src/loader.cpp](../src/loader.cpp) | 把 `<gameRoot>/tc-modloader-data/gate-delay.txt` 交给 `NativeRuntime` |
| [tools/gate-delay-boards.py](../tools/gate-delay-boards.py) | 造板子：剪开版 ring/latch、以及**玩家式闭合环** |
| [tools/gate-delay-survey.py](../tools/gate-delay-survey.py) | 只读分析真机转储（块切分、读/写槽、LATE/副作用分类） |
| [tests/gate-delay.cpp](../tests/gate-delay.cpp) + [.ps1](../tests/gate-delay.ps1) | 离线用例（4 个场景），已进 `tools/test.ps1`（id `gate-delay`） |
| [tests/gate-delay-cycle-playtest.ps1](../tests/gate-delay-cycle-playtest.ps1) | 真机：剪开板 → 编译 → 接回（M0/M1 那套） |
| [tests/gate-delay-ring-playtest.ps1](../tests/gate-delay-ring-playtest.ps1) | 真机：环振/锁存/开关/关卡门控，逐周期读状态槽 |
| [tests/component-cost-probe.cpp](../tests/component-cost-probe.cpp) | 既有探针；本文给它加了 `TC_GATE_DELAY_SAMPLE=1`（逐周期扫槽） |

## 3. 已验证（真机证据行）

| 项 | 证据 |
|---|---|
| 改写后能编译运行（分层档） | 同一条命令下改写次数 657 → **1**；探针 `after sim: cycle=8`；`Gate delay: units=8 writes=14 commits=12` |
| 运行时按单位推进 | `units=7 writes=78 commits=65 reads=36 pending=13`（与转储逐条对得上） |
| **环合法**（剪开→接回） | `Gate delay: re-closed 2001<-2003 - slot 262 reads slot 268`；改写后 `A=~(read(268)&…)`、`B=~(read(264)&…)`、`C=~(read(266)&…)` |
| **环会振荡** | `TC_GATE_DELAY_K=7` 时 `changed slots=5`，门槽序列 `1,1,1,0,1,1,0,0,1,1,1,0` |
| **环能锁住** | latch 板 `changed slots=0`，稳态 `A=1,B=0`（同一探针、同一读数路径的对照） |
| 设置页开关 + 持久化 | 不设任何环境变量、只靠 `gate-delay.txt`：`Gate delay: switch on (saved setting)`；`on=0` 时同一块板子静止且无 `rewrite done` |
| 只在沙盒 | `Level loaded: sandbox` 才改写；门控是"关卡名含 sandbox" |
| 事件档（M1b） | `sched=event, guarded=6 source=9`；真机计数与分层档逐项相同，单位级追踪振荡模式一致 |
| **混合板也剪环**（玩家现场：两个 NAND 的触发器） | `board has 7 components, 4 wires, 4 directed NAND/constant connections` → `cut wire 1 between 2001 and 2002` → `cut wire re-closed 2001 <- 2002` → `gates=4 delay=1 flag=0`，12 周期 `changed slots=0`（锁存稳态） |
| 同板无关元件不影响剪环 | 上面那块板同时有 2 个 AND、1 个墓碑；它们既不产生边，也不阻止剪环（离线断言 + 真机） |
| **环检测钩子不再打死游戏** | 修好参数转发后：`loop-through-and` 板（环经过 AND，剪不掉）在旧构建下于 `contains(custom_prototype)` 崩溃（事件日志 `Turing Complete.exe 0x588dc`），新构建下 `flag=1` 正常结束；元件工坊 `foundry` 加载玩家式板同样正常（§5.6、计划 §27） |
| **换门也剪**（NOR/AND/OR/XOR/…） | `board has 5 components, 4 wires, 2 directed combinational connections` → `cut wire 1 between 2301 and 2302` → `re-closed 2301 <- 2302` → `gates=2 delay=1 flag=0`（NOR 锁存）；同一批还造了 or/xor/and 三种板（§5.7、计划 §28） |

## 4. 怎么跑（接手第一组命令）

```powershell
# 构建（约 5 分钟；改过 src 就要重建，否则跑的是旧 dll）
.\build.ps1

# 离线用例（快，先跑这个）
.\tests\gate-delay.ps1            # 4 个场景
.\tests\gate-delay.ps1 -Dumps     # 再把 build/ 下 10+ 份真机转储跑一遍
.\tools\test.ps1 -Tier fast -Name gate-delay -NoBuild

# 造板子（常量模板用任意一块含 com_constant 的板子，例如 campaign 里某关的 circuit.data）
python tools\gate-delay-boards.py --constant-template <带 0x2e 元件的 circuit.data>

# 真机：环振（默认 ring 板；K 用奇数才看得见交替）
$env:TC_GATE_DELAY_K='7'
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on

# 真机：锁存（对照：应当 changed slots=0）
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on -Board build\gate_delay_latch_board.data `
                                     -Close 2101:2103:2102

# 真机：**玩家式闭合环**——第 1 项的复现命令（§5.1 之后应当自动剪开并振荡）
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on `
      -Board build\gate_delay_ring_closed_board.data -Close ''

# 真机：**两个 NAND 的触发器**（玩家现场那块混合板：2 NAND + 2 常量 + 2 AND，闭环）
python tools\gate-delay-boards.py --template build\c2_cut_board.data `
       --constant-template build\gate_delay_latch_board.data --and-template build\and2_component.data
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on `
      -Board build\gate_delay_two_nand_board.data -Close '' -Sandbox gate-delay-two-nand-fix2

# 真机：**环剪不掉**的板子（环经过 AND）——环检测钩子丢参数的复现板，必须不崩、给出 flag=1
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on `
      -Board build\gate_delay_loop_through_and_board.data -Close '' -Sandbox gate-delay-loop-and-repro

# 真机：**换成别的门**（NOR 锁存；同批还有 or/xor/and 三种板），必须同样被剪开+接回
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on `
      -Board build\gate_delay_nor_latch_board.data -Close '' -Sandbox gate-delay-nor-latch

# 真机：**环经过自定义元件**（剪不掉 → 交给游戏环检测 → 必须只是 flag=1，不能崩）
.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on `
      -Board build\gate_delay_loop_through_custom_board.data -Close '' -Sandbox gate-delay-custom-loop

# 真机：**元件工坊**（level=foundry，工坊板子 = schematics\foundry\<名字>\circuit.data）
$env:TC_FIXTURE=(Resolve-Path build\and2_component.data).Path
$env:TC_SOLUTION=(Resolve-Path build\gate_delay_user_board.data).Path
$env:TC_COST_MODES='plain'; $env:TC_LEVEL='foundry'
.\tests\component-cost-playtest.ps1
```

### 开关与环境变量

| 变量 | 作用 |
|---|---|
| `TC_GATE_DELAY=1` | 打开模式（等价于设置页勾选；设置页写的是 `tc-modloader-data/gate-delay.txt`） |
| `TC_GATE_DELAY_K=<n>` | 每周期单位数（默认 8） |
| `TC_GATE_DELAY_SCHED=event` | 事件档（按输入变化求值，含真毛刺）；默认是分层档 |
| `TC_GATE_DELAY_CLOSE=a:b:c` | 手工接回记录 `消费者:占位常量:生产者`（旧路径，仍需离线剪开的板子） |
| `TC_GATE_DELAY_TRACE=1` | 前 16 个单位的 `state`/`snapshot` 追踪（看环到底动没动最快的办法） |
| `TC_GATE_DELAY_SAMPLE=1` | 探针侧逐周期扫 512 个状态槽（`component-cost-probe`） |
| `TC_MODLOADER_DUMP_SOURCE=1` | 转储生成源码；改写**之后**的转储是 `native-logic-source-gated*.txt` |

## 5. 闭环自动剪开已完成（含混合板）；只剩"环穿过未知引脚元件"

### 5.1 小型离散门闭合环（第 1、2 项）✅

已完成。旧卡点不是长线本身，而是把 `preorder` 的非导线参数误当成 `0x68` 导线表；那份数据里
甚至能看到 `campaign` 字符串，所以 `8 wires / 2 connections` 是假阳性。现在直接钩游戏自己的
`wires__modelZsave95mongerZcommon_u4073`：它在 `preorder` 前生成规范化临时导线序列，完整折线的
两端已经在 `+0x18..+0x1e`。loader 只剪这份临时副本，玩家棋盘与存档不变。

同时修正了原点 kind 0 墓碑与真实 NAND `(0,0)` 的最近点并列：墓碑不再参与引脚归属。
闭合三 NAND 环的真机证据：

```text
Gate delay: board has 7 components, 6 wires, 6 connections
Gate delay: cut wire 2 between 2002 and 2001 so the compile sees an acyclic board
Gate delay: cut the copied wire path before preorder consumes it
Gate delay: cut wire re-closed 2002 <- 2001 (consumer input now reads slot 266)
Gate delay: rewrite done; ... recut=1 ... K=7
```

结果为 `compiled gates=3`、`flag=0`、`changed slots=5`；默认 layered 与
`TC_GATE_DELAY_SCHED=event` 两档都通过。离线新增 `checkCycleCut`，专门断言同坐标墓碑不会成为
剪边端点。复现命令仍是 §4 最后一条，再额外设 `$env:TC_GATE_DELAY_K='7'` 以避免周期采样相位混叠。

### 5.2 其它

| 项 | 说明 |
|---|---|
| M4 诊断面板（可选） | 未做；`TC_GATE_DELAY_TRACE` 已经覆盖一半 |
| K 玩家可调 | V2；现在固定 8，只能环境变量覆盖 |
| 时钟语义 B（信号驱动周期） | V1 走"周期由模拟器给"，见计划 §12.1 |
| 带环存档在普通逻辑下的策略 | 计划 §2 的三选一，待拍板 |
| 自定义元件 | 门级延迟下**晚一个周期**（桥接回调解在块内只跑一次）；`com_cc_input_buffer` 等中转元件按 1 单位 |
| 复杂/混合板自动剪环 | **已做**（§5.5、计划 §26）：引脚方向来自精确引脚，环来自有向图，含自定义/其它门的板照样剪。剩下的门槛只是工作量上限（`>4096` 元件或 `>16384` 导线才跳过，日志写 `too large to scan here`），以及"环穿过引脚未知的元件"仍然剪不了 |

### 5.3 CPU 沙盒编译风暴已修复

真实 CPU 板曾因三件事叠加而未响应：无向图误剪普通连接、宿主声明与发布被拆出词法作用域、
多驱动累加器左值被替换成 `tc_delay_read`。现在歧义回接整轮拒绝（只有环穿过**引脚未知**的
元件时才跳过剪环，见 §5.5）；宿主块整体守卫；被复合赋值的早先节点变量保持本地。
用户的 `RV32I 1` 与 `RV32I` 存档副本均在
saved setting 开启、K=8 下真机编译并推进到 cycle 1，分别得到 79,763 与 67,183 gates，
两者均 `delay=202`、`flag=0`。

### 5.4 沙盒编辑闪退已修复

编辑中的小型中间板曾被近邻无向图误剪。剪线如果发生在 `preorder` 内部的 wire-copy 钩子，旧代码
没有在返回时恢复临时线；序列释放后 `cutState.wire` 成为悬空指针，同时拒绝改写又把旧
`pendingCuts` 留给下一轮编译。后续重编译/保存可能写已释放内存，表现为修改电路后直接退出。

现在 `preorder` 返回无条件恢复临时线；改写成功、拒绝与关卡切换都通过 `finishCompileCut()` 清理
本轮记录。离线 `gate-delay.ps1` 全过；开启延迟的真机组件编辑测试完成新增、删除、事务、
撤销和重做，闭合三 NAND 环仍可编译运行；玩家当前 `RV32I 1` 存档副本也以 66,963 gates、
delay 202、`flag=0` 推进到 cycle 1。

### 5.5 混合板整板跳过已取消：精确引脚 + 有向图（2026-09-24 第二轮）

玩家搭完两个 NAND 的触发器后窗口再次无响应。现场存档解出来是 **2 个 NAND（`0x06`）+ 3 个
自定义 AND2 实例（`0x4e`）**；日志每帧只有 `automatic cut skipped for a mixed circuit
(unsupported component kind 78/99/231)`，一次 `rewrite done` 都没有。根因是 §5.4 那条边界：
"板上有 NAND/常量之外的元件 → 整板不剪"，而玩家的板子总是同时放着别的元件，于是真环永远剪不掉，
游戏自己的环检测每帧拒绝编译（与 §5.2 同一形态）。

现在自动剪环只看**精确引脚**：`0x06` 的输入 `(-1,-1)`/`(-1,1)`、输出 `(2,0)`，`0x2e` 的输出
`(3,0)`，带旋转；其它 kind 一律不产生边。引脚先收集成坐标索引（不再是每条导线端扫全部元件），
所以工作量上限从 16/64 放宽到 4096/16384；两枚引脚同格记为冲突、不猜。环由有向 DFS 找，
剪开时记录 `消费者:生产者:消费者输入序号`，回接不再依赖"哪个门只有一个零输入"。

验收（`build/gate_delay_two_nand_board.data`，2 NAND + 2 常量 + 2 AND = 7 元件 4 线）：

```text
board has 7 components, 4 wires, 4 directed NAND/constant connections
cut wire 1 between 2001 and 2002 so the compile sees an acyclic board
cut wire re-closed 2001 <- 2002 (consumer input now reads slot 262)
rewrite done; wave=14 … recut=1 … sched=layered, K=8
gates=4 delay=1 flag=0 / after sim: cycle=11 / changed slots=0（锁存稳态）
```

复现：`python tools\gate-delay-boards.py …`（见计划 §26.4）后
`.\tests\gate-delay-ring-playtest.ps1 -SavedSetting on -Board build\gate_delay_two_nand_board.data -Close ''`。
闭合三 NAND 环回归照旧通过（`gates=3 delay=2 flag=0`，槽 265/267 持续翻转）。当前部署 SHA-256：
`74DC16E0403040D42A84E9BFD37727270C2B2B03C503A5A33612D85A4851F97D`。

**仍然剪不了的形态**：环是**穿过**引脚未知的元件闭合的（自定义元件内部、复合件）。顶层没有
可剪的边，游戏自己的环检测仍会拦住编译。要覆盖它得把自定义元件展开成内部网表——这是下一步，
不是"再调大阈值"能解决的。

### 5.6 玩家"未响应"其实是崩溃：环检测钩子丢了两个参数（2026-09-24 第三轮）

玩家把沙盒里跑通的触发器复制进**元件工坊**后又"未响应"。这次查到了真凶，而且**不是**
"编译器每帧重试"（§5.2/§5.5 之前的说法要按本节更正）：

- Windows 事件日志写着 `Turing Complete.exe` 异常 `0xc0000005`、偏移 `0x17a1a0`；
  该偏移正是游戏自己的 `find_circular_path`（`0x14017a0e0`）+0xC0，也就是**我们自己挂钩的
  那个环检测函数**。12:17 的那次"无响应"是同一个偏移；
- 那个函数有**四个**参数（隐藏结果指针、序列、组件 id、板子的自定义原型表），而 detour 当时
  声明成两个参数：`r8`/`r9` 留在 detour 里被别的东西覆盖，游戏再拿它当指针用 →
  在 `find_circular_path` 里读 `[垃圾+8]`，或下一层死在 `contains(custom_prototype)`；
  `fault.log` 里的 `0x0000000000000f`、`0x549`、`0x20180406101a` 就是那些残留值；
- 修复：两个 detour 改成四参数、四个寄存器原样转发（`set_circular_dependency` 实际只用第一个
  参数，多转发无害）；四个 gate-delay 原始 detour 都加上 `fault::Scope`，以后 `fault.log`
  会点名是哪个钩子，而不是"outside any plugin callback"。

对照复现：`build/gate_delay_loop_through_and_board.data`（`NAND1 -> NAND2 -> AND -> NAND1`，
环经过 AND 所以自动剪环看不到它）在沙盒 + 开关 on 下，旧构建崩在 `contains(custom_prototype)`，
新构建给出游戏自己的 `flag=1` 并正常跑完；元件工坊（`foundry`，板子存在
`schematics\foundry\<名字>\circuit.data`）加载玩家式板（2 NAND 锁存 + 3 个自定义 AND2 实例，
`build/gate_delay_user_board.data`）也正常。部署 SHA-256：
`50B0B9B3A592DC6C876B2272438D0B14F6114C1415AA42F389B28E95E23FA0A9`。

**教训**：给游戏函数挂原始 detour 前，先反汇编序言数参数，全部原样转发。漏转发不会在离线
用例里暴露，只会随寄存器残留概率性地打死游戏。定位路径：事件日志（模块+偏移）→
`objdump -d --start-address=0x140000000+偏移`。

### 5.7 不只 NAND：通用组合逻辑引脚表（2026-09-24 第四轮）

玩家把锁存里的 NAND 换成 NOR/AND/OR 之类后，游戏又报"循环依赖"——那是能力边界，不是回归：
自动剪环原先只认 `0x06`/`0x2e`。现在引脚几何来自**游戏自己的原型表**：

- `tools/generate-gate-pins.py` 把 `build/kinds.txt`（125 个内建原型）转成
  `src/gate_pins.generated.hpp`——49 个**组合逻辑**原型；有状态/驱动外界的（Delay/Register/
  Counter/Switch/RAM/Probe/Screen/Keyboard/Time/Push Button/Static Evaluator/Level Component）
  与自定义元件、自定义引脚都不进表。**经过状态元件的环游戏自己就能编译，剪它会把能跑的电路
  剪坏**，所以不在范围内。
- 新增约束：生产者有多于一个输出的边不能剪（回接记录只能写元件 id，写不出哪个输出）；
  DFS 跳过这种回边继续找，找不到就把根节点倒序再找一轮。
- 日志措辞：`directed NAND/constant connections` → `directed combinational connections`。
- 崩溃回归板随之换成 `build/gate_delay_loop_through_custom_board.data`（环经过自定义元件，
  仍会交给游戏环检测；§27 的 `loop-through-and` 板现在会被正常剪开）。

真机（部署构建）：NOR 锁存 `board has 5 components, 4 wires, 2 directed combinational
connections` → `cut wire 1 between 2301 and 2302` → `cut wire re-closed 2301 <- 2302` →
`gates=2 delay=1 flag=0`；两个 NAND 锁存 `gates=4 delay=1 flag=0` 稳态；三 NAND 环 K=7
`changed slots=5`；自定义环板 `flag=1` 不崩。部署 SHA-256：
`9425577901C57ECBF1EFB33760048D89E7B81CC1A601BA110B61A3369019CB26`。

仍然不剪：经过自定义元件的环（实例内部网表不在板子上）、以及"两轮搜索只有多输出生产者回边"
的环（拒绝而不是乱写记录）。

### 5.8 时钟源元件（2026-09-24 第五轮）

模式是逐单位的，而游戏自己的源只按周期动——所以加了 `examples/clock`（Mod `local.clock`）：
声明式注册的纯源元件（0 输入、1 位输出、1 状态字、`gate_cost=0`、`delay=1`），**关模式时**是
每周期翻转的方波；**沙盒 + 模式开**时，延迟模型把它那个槽的读值换成"每周期开头 1 个单位宽的
脉冲"（`src/gate_delay.hpp` 的 "the clock source" 段），于是脉冲会按 §3.1 逐单位传播、能被门搭
latch 捕获，也能被漏采。

两个坑值得记：

1. 时钟实例的 `com_custom` 块在**桥接替换之后是空的**（值行已经变成 `tc_logic_invoke/peek`），
   所以"从块里读槽号"永远失败——槽号要从**桥接调用行的变量名**取；而且桥接替换发生在 gate-delay
   改写**之前**，要在改写里按 token 找（token→原型 id 的对应在原生逻辑绑定处记下）。
2. 别指望编译期的 preorder 扫描先跑：实测那次改写跑在它前面，板子上有没有钟得在改写时自己从
   `boardModel()+0x78/0x98` 扫一遍。

验证（真机 `build/gate_delay_clock_board.data` = 时钟 + NOT，`TC_GATE_DELAY_TRACE=1`）：
逐周期 `slot256=0,1,0,1` / `slot258=1,0,1,0`；逐单位 trace 里 NOT 的槽 `unit=1` 为 0、之后为 1
——单单位脉冲被读到并反相发布。离线用例 `checkClockPulse`。

脉冲宽度/分频目前固定，要让玩家可调得接 `config_schema` + `tc_component.storage`（计划 §29.4）。

### 5.9 对称环的确定性破平局（2026-09-24 第六轮）

玩家报告"两 NAND 锁存，输入 00 或 11 时两个输出都是 1"。查下来：**00 → 1,1 是正确行为**
（NAND 锁存低有效，00 就是双置位）；**11 是模型的对称性问题**——两个门完全相同、每单位从同一
快照求值再一起提交，没有不对称来源，于是同相翻转 `1,1 ↔ 0,0`，而 K=8 的逐周期采样恰好同相位，
看起来像卡在 1,1。

规则（零稳态开销）：**同一反馈环上的所有节点在同一单位里都变成相同新值时，板序最靠前的那个
保持旧值一格**。只有"整个环落进同步"的那一格命中，稳定的锁存、普通电路、奇数环振都不受影响
（不是"固定加 1 单位"，也不是"细分到 0.5 单位"）。编译期用已有有向图跑 Tarjan 求强连通分量
得到平局组（板序排序），改写时把组员的槽填进运行时，`commitPending()` 在提交那一格执行裁决。

真机：11 → 稳定 (0,1)；00 → 1,1；01 → 稳定 (1,0)；三 NAND 环 K=7 仍振荡（`changed slots=5`）。
离线 `checkTieBreak`。部署 SHA-256 前缀 `1520673FAC78D282`，备份
`game_engine.dll.bak-20260924-before-tiebreak`。

### 5.10 按元件延迟表（2026-09-24 第七轮）

原来"所有非状态元件都是 1 单位"，把非门和与门当成一样快。现在按 CMOS 的级数给表（`kindDelay(kind,bits)`）：
NOT/NAND/NOR = 1；AND/OR/Mux/Concatenator = 2（门 + 反相器）；XOR/XNOR/Equal/Less = 3；
Negate/Inc 2+级、Add/移位 3+级、CLZ/CTZ 4+级、Mul 6+级、Div/Mod 8+级（"级"按位宽 ≤4/8/16/32+
加 0/1/2/3）；Delay Line/RAM/Register/Counter 仍 4；其余保守 1。**是工程值不是纳秒**；
`TC_GATE_DELAY_TABLE=flat` 回到旧表做对照。

离线 `checkDelayTable`（表值 + 确认写进了生成程序的 `U64 2` 等）；真机：两 NAND 锁存不受影响
（NAND=1）仍稳定 (0,1)，`NAND→NAND→AND` 环板正常编译。部署 SHA-256 前缀 `4070806CB5DA590E`，
备份 `game_engine.dll.bak-20260924-before-delay-table`。

### 5.11 单位步进：一个周期内看见多次变化（2026-09-24 第八轮）

玩家反复遇到同一个根本问题：**棋盘每个 program pass 只更新一次**，而一个 pass 跑 K 个单位，
所以周期内部的逐单位变化谁也看不见（"非门2 没反应""与门状态不变""低电平不红"全出在这里）。

现在加了一档**单位步进**：开 `tc-modloader-data/gate-delay-step.txt`（`on=1`，或环境变量
`TC_GATE_DELAY_STEP=1`）后，**一次 pass 只推进 1 个单位**——棋盘每推进一次你就看到一次状态变化，
8 次才等于原来一个周期；可以单步，也可以让它慢慢跑。实现是把改写生成的
`var tc_unit = <K>` 换成 `Int (U64 game_engine.'tc_delay_units_this_pass'())`（宿主函数按设置
返回 K 或 1；`time` 仍然单调，所以周期内单位序号和时钟脉冲语义不变）。

证据：同一块 `edge_detector` 板，开关前探针每个周期读到的与门恒 0，开关后读到 `1,0,1`（在动）；
日志里 `refresh=` 每 pass 只 +1（原来 +8）。部署 SHA-256 前缀 `9FDCBFAEE32A7AD5`，
备份 `game_engine.dll.bak-20260924-before-unitstep`。

### 5.12 时钟的绘制与逻辑统一（2026-09-24 第九轮）

玩家报告"屏幕上的时钟线和门收到的时钟不是一个值"——是真的：**状态数组里放的是时钟元件回调的
每周期翻转值**（周期 2 个模拟周期，导线着色照着它画），而门通过 `tc_delay_read` 读到的是延迟
模型合成的**每周期 4 高 4 低**脉冲（周期 1 个模拟周期）。另外发射器把同一个输出写进元件节点槽
和每根相连导线的槽（时钟板上是 256/257），此前只有桥接 token 那条路径标记别名槽。

修法：编译期 `markClockValueSlots()` 统一标记时钟值的**全部**输出槽（值行名字里的槽号 + 所有由
该值 store 的槽），两条来源共用；运行期 `publishClockPulse()` 在每个单位的提交之后、快照之前，
把 `tc_delay_read` 用的那同一个 `clockPulse()` 直接写进这些槽。**绘制、导线、门从此读同一个
函数、同一份状态。**

真机（`build/gate_delay_clock_board.data`，`TC_GATE_DELAY_STEP=1` + `TC_GATE_DELAY_TRACE=1`）逐
单位 trace：绘制槽 256/257 = `1,1,1,0,0,0,0,1`（4 高 4 低），NOT 槽 = `0,0,0,0,1,1,1,1`
（绘制槽反相延迟一格）；日志 `2 clock slot(s) answer with a 4-unit pulse`。离线 `checkClockPulse`
断言两个槽都被标记且都被发布。非步进时一帧落在周期最后一个单位（脉冲高段），线看起来恒亮——
与同一瞬间门读到的一致；波形要看就开单位步进。部署 SHA-256 前缀 `0C8DCDD8C51A02AD`，备份
`game_engine.dll.bak-20260924-before-clockpublish`。

### 5.13 显示粒度：单位步进会让一个周期重画 8 次（2026-09-24 第十轮）

玩家问"现在时钟源是一个周期好几个方波吗"、并报"单周期手动步进时一个周期很多闪烁"。用新增的
**`TC_GATE_DELAY_TRACEALL=1`**（`src/gate_delay.hpp` 的 `traceEveryUnit()`：每个单位一行，带
`site` / `owner` / 单位序号 / 脉冲 / 时钟槽 / `GetTickCount64` 毫秒）在沙盒里量：

- **信号本身没变**：逐单位 `pulse` 恒为 `1,1,1,0,0,0,0,1` 循环 = 一个周期一次上升、一次下降，
  4 高 4 低；时钟槽与它一致（`slot256` 同步，`slot258` = 反相延迟一格）。
- **闪烁是显示粒度**：`gate-delay-step.txt on=1` 时两次单位推进间隔 **15.6 ms（≈每帧一个单位）**，
  一个周期 = **8 次重画**；步进关时 8 个单位落在**同一毫秒**（整周期一帧）→ 一个周期 1 次重画。
  原因是开了步进后 pass 只跑 1 个单位就交回前端，下一帧再调一次——这就是"逐单位慢慢跑"的实现；
  §32 的修复让时钟线也加入这次演进，于是它从"每 2 个周期变一次"变成"每周期变两次"。
- 想要"点一下 = 一个周期 = 一次重画"：`gate-delay-step.txt` 写 `on=0`。

沙盒里"空转"（不下任何步进命令）不会推进单位时钟：探针只请求 8 个周期时，日志正好 8×8 个单位，
没有多余的行。仓库里另新增 `tools/Prune-Caches.ps1`（插件解包缓存与 `*.mod.bak-*` 快照的清理，
带 `-DryRun`），`tools/Clean-Build.ps1` 改成清扫 `build/` 下所有目录（旧模式表漏掉 punch-tape /
word-watchee / save-test / `test-<guid>` / `gate-delay-*`，工作副本因此长到 140 GB）。
部署 SHA-256 前缀 `3551FA373D75BB39`，备份 `game_engine.dll.bak-20260924-before-traceall`。

### 5.14 时钟的两种形状 + 单位步进慢放（2026-09-24 第十一轮）

玩家报"单步执行时时钟源闪很多下"，把单位步进关掉后又报"时钟源不动了"。两次都对，原因是一条
**采样事实**：一帧只采样一个单位，按周期步进时每次都落在同一个单位上，所以**周期恰为一个周期的
信号必然显示成常数**——半周期脉冲（4 高 4 低）在按周期步进的画面上就是恒高。于是给了两个旋钮，
都能在游戏里 Options → General 直接改（也都写回文件）：

| 旋钮 | 值 | 效果 | 文件 |
|---|---|---|---|
| 单位步进（慢放） | 开 + `ms=100` | 一个 pass 只推进一个单位，且**按毫秒节流**：太早来的 pass 推进 0 个单位、画面保持上一单位——于是单步会像慢动作演一遍 4 高 4 低，而不是每帧一格的闪烁（原来 15.6 ms/单位） | `gate-delay-step.txt`：`on=1`、`ms=N`（`ms=0` 回到每帧） |
| 时钟形状 | 慢时钟 | 时钟不再"每周期一个脉冲"，而是**每周期翻转一次**的方波（高一个周期、低一个周期）：按周期步进时每次都能看见它变；门读到的仍是同一个函数算出的同一个值（§32 的规则不破） | `gate-delay-clock.txt`：`slow=1`（`units=N` 可另外钉脉冲宽度，`0`=半周期） |

实现：`clockFlipsPerCycle()` 让 `clockPulse()` 返回 `(time/K)%2` 取反的形状（`pulse()` 仍是
绘制、导线、`tc_delay_read` 唯一的值来源）；`unitPaceMs()` + `lastUnitTick()` 在
`unitsThisPass()` 里做节流，节流时返回 0 个单位（pass 不做事，前端下一帧再调）。日志每次编译都
写明：`unit step=off (one pass = one cycle), clock=one square wave (flips once per cycle)`。

真机验收（时钟板，`TC_GATE_DELAY_TRACEALL=1`）：

- `slow=1` + 步进关：逐单位 `pulse` 为**整周期恒值**（`t=1..7` 高、`t=8..15` 低、`t=16` 又高），
  逐周期采样里时钟槽 `0,1,1,0,1,1,0,1,0,1,0,1`（在动），NOT 槽是它的反相延迟；日志
  `Gate delay: clock flips once per cycle (slow square wave)`。
- 默认（`slow=0`）仍是一个周期一个 4 高 4 低脉冲，按周期步进时显示恒高——要看得开单位步进（慢放）。

玩家侧配置现在就是"慢时钟 + 步进关"，即单步一次看见时钟翻转；想逐单位看传播就勾上"逐单位步进"。
部署 SHA-256 前缀 `17538CAE7283BEC1`，备份 `game_engine.dll.bak-20260924-before-slowclock`。

### 5.15 导线拷贝不占延迟（2026-09-24 第十三轮）

玩家选了"给导线去掉延迟"这条路：发射器会把一根线变成自己的节点（`com_cc_input_buffer`
以及凡是需要给消费者镜像一根线的地方），而延迟表给**每个**节点一格——对线来说是错的。棋盘画的正是
那个**拷贝槽**，而旁边的门引脚是**源槽**（元件面板读的也是引脚），于是线会一直停在上一格的值上，
只要电路还在变它就永远慢一拍。

现在纯拷贝节点改成**镜像**：`pureCopySource()` 认"整块就是一次槽读、没有任何运算符"的块，
把它的 `tc_delay_write` 换成宿主调用 `tc_delay_mirror(dst, src, bits)`；`applyMirrors()` 在
每个单位的提交之后、快照之前，把源槽的字节原样写进拷贝槽（两遍，处理线接线的链），值变了就标脏。
会计算的块（门、splitter 的位提取、常量）一律保持原来的延迟。离线新增 `checkWireMirror`：
产物里是 `tc_delay_mirror'(U64 258, U64 256, U64 1)`、没有 `tc_delay_write'(U64 258`，
且运行时源槽变化后拷贝槽**同一单位**就跟上。

注意：这条修正只对"有源槽的拷贝"起作用。玩家当前那块板（活动存档 `架构/1`）编译产物里六个
`com_cc_input_buffer` 都是**常量 0**（模式开、关都一样），所以那条线上没有拷贝延迟可去；板里的
门与回接（`~read(266) & read(292)`）逐单位都在按模型走。部署 SHA-256 前缀 `BA5BFE1E683D8433`，
备份 `game_engine.dll.bak-20260924-before-wiremirror`。

## 6. 必须知道的机制（少走弯路）

1. **两个 cycle body 机制不同**（计划 §17.1）：`mode_refresh` 用状态槽（`load/store
   #SIMULATION_STATE`），`mode_run` 的突发体用**局部变量**波前（`var vidN = … vidM …`）。
   **节点变量名里的数字就是它的状态槽偏移**（`vid308` ↔ 槽 308）——这条省掉了"元件↔槽"映射。
2. **LATE 块不能搬出单位循环**：它读的是本周期波前的局部变量，搬出去就是未声明标识符。
   现在的做法是留在循环里用 `if tc_unit == 0 { … }` 守卫（同理：桥接回调、示波器 tick）。
3. **外调必须带显式类型前缀**：`(U64 game_engine.'f'(…)) != 0`。写成裸调用会被游戏前端断言
   `front_end.nim(2749, 13) lhs.exp.info.value.kind != exp_none` → **程序根本不生成**
   （表现是"编译期看不出问题、运行时一次都不跑"）。这个坑踩过两次。
4. **振荡周期是 2 个单位**（同步更新让整个环一起翻转），所以 `K=8` 是整数倍 →
   逐周期采样永远同一相位、看起来"不动"。要看它动：`K=7`，或用 `TC_GATE_DELAY_TRACE=1`。
5. **探针那条通道不是权威读数**（它靠 `run_to` + 轮询，停在哪取决于仿真线程进度）；
   权威是程序里 tick 处的采样（`tc_scope_tick` / `TC_SERVICE_CAPTURE`）与运行时日志。
6. **环被检测到之后，编译根本不会发射电路**（§13.3/§13.5）；而"改模型"在 `level.load`
   那一刻已经太晚（§13.6）——所以自动剪开必须发生在建图/环检测**之前**（§22 的挂点）。
7. 改写器不猜顺序：接回靠"**消费者块里那个被折叠的常量操作数**"定位（无占位常量路径），
   或靠"占位常量的槽读"（旧路径）。多个折叠操作数时**拒绝改写并报错**，不猜。

## 7. 踩坑清单

- **原始 detour 必须原样转发全部参数寄存器**：`find_circular_path` 有四个参数，我们只声明
  两个 → 游戏拿着 `r8`/`r9` 的残留值当指针，在环检测里崩溃（§5.6、计划 §27）。挂新钩子前先
  反编译序言数参数；"未响应"先查事件日志的模块+偏移，不要先假设是重试风暴。
- **改完 src 必须重建**：跑真机前先 `.\build.ps1`，否则用的是旧 `dist\tc-loader.dll`（这一轮因此白跑过一轮）。
- **playtest 的沙盒目录**在 `build/<name>`；前一次的游戏进程没退干净会让 `Remove-Item` 失败，
  换一个名字即可：`-Sandbox gate-delay-ring2`。残留进程按路径杀：
  `Get-Process "Turing Complete" | Where-Object { $_.Path -like "*tc-modloader\build*" } | Stop-Process -Force`。
- **component-cost 探针会自己 `load_level("sandbox")`**：拿它去验"非沙盒关卡不受影响"是无效的
  （它自己会切到沙盒）。要验红线 1 请用加载真实关卡的用例（例如 `simulation-state`）。
- **剪开状态绝不能进存档**：三个还原点已就位，但这是"新增挂点时必须复查"的清单项。
- **只支持指定构建**：所有挂点都走符号别名（`preorder__modelZsimulationZpreorder_u31266`、
  `find_circular_path_…u27493`、`set_circular_dependency_…u27466`、`options.general`、`load_level`）。
  换了游戏版本时按顺序降级：缺哪个就在日志里写明"某某恢复原样"，模式不生效但**不会更糟**
  （新构建的符号名要加进 `src/symbol_profile.hpp` 与 `compat/profiles.json`）。
- 转储有**两份**语义：`native-logic-source-N.txt` 是改写前，`native-logic-source-gated*.txt` 是改写后；
  看改写结果只能看后者。
- `K` 与环周期互质才看得见振荡（见 §6.4），别把"看不见"当成"没振荡"。

## 8. 工作树状态

改动**未提交**，都在磁盘上：

```text
M  src/loader.cpp, src/native.hpp, src/component_timing.hpp, src/native_logic.hpp,
   tests/component-cost-probe.cpp, tests/gate-delay.cpp, tests/test-catalog.json
?? src/gate_delay.hpp, tools/gate-delay-boards.py, tools/gate-delay-survey.py,
   src/gate_pins.generated.hpp, tools/generate-gate-pins.py,
   tools/gate_delay_rewrite.py（M1a 的 Python 原型，仅参考）,
   tests/gate-delay.cpp, tests/gate-delay.ps1,
   tests/gate-delay-cycle-playtest.ps1, tests/gate-delay-ring-playtest.ps1,
   tests/gate-delay-slots-probe.cpp（**未跑通**：它拿不到 board model，正文明记；实际观测改用
   component-cost-probe 的采样开关）,
   docs/PLAN-gate-delay.md, docs/HANDOFF-gate-delay.md（本文）
```

默认行为不受影响：不设环境变量、设置页开关关闭时，沙盒与普通关卡都与未装 Mod 一致。
