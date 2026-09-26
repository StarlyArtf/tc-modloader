# 计划：门级延迟的调度器替换（事件驱动 + 精细时基）

日期：2026-09-25。状态：**方案，未开工**（M0 度量可先跑，不改代码）。
**定位**：已被 [PLAN-sandbox-simulator.md](PLAN-sandbox-simulator.md) 取代为主线方案，
本文保留为**备选/退路**（改动面最小的渐进路线：不动网表、不动器件语义）。它的 §6 对拍门槛、
§2 成本模型与 §5 的 M0 度量方法在新方案里同样适用。
构建：该指定构建（EXE 基址 `0x140000000`，引擎 `tc_game_engine.dll` 基址 `0x180000000`）。
范围：**只换调度器**——保留游戏为每个元件发射的求值代码，保留全部既有宿主接口与红线。

> 一句话：把"一周期跑 K 个单位、每单位一次全状态快照 + 提交队扫描"换成"按事件时刻推进"，
> 于是时间分辨率与 K 解绑（可以细到亚单位），每 pass 的成本从 ∝ K 变成 ∝ 真正发生的事件，
> 闲置电路几乎免费；元件语义、延迟数值表、剪开/接回、开关与沙盒门控全部不动。

## 1. 目标与非目标

### 目标

1. **时间分辨率与 K 解绑**：内部分辨率到亚单位（默认 1 单位 = 16 滴答），K 仍是"一周期几个
   单位"，但不再决定成本。
2. **成本与活动率挂钩**：不再每单位全板快照/扫描；大板 CPU 占用按"真的动了多少"收费。
3. **惯性延迟**（可开关，默认关）：去掉"每次求值都排一条、到期必落地"这一 transport 语义，
   支持"窗口内被新值取代"与"窄于门延迟的脉冲被吞掉"。
4. **时钟成为事件源**：脉冲的两次变化是周期内的两个预定事件，而不是每批扫全部时钟槽。
5. **既有语义一条不改**：先算后提交、按器件延迟发布、环合法、锁存/振荡、剪开/接回、
   对称破平局、LATE/寄存器/关卡 IO/绘制发布的周期边界、开关关闭时逐字节一致。

### 非目标（本方案不做）

| 不做 | 归谁 | 原因 |
|---|---|---|
| 改延迟**数值**表（接 `get_delay_cost` / STA / 每实例失配） | 并列的另一期 | 本方案改的是"什么时候用这个数"，不是"这个数是多少"；两件事独立验收 |
| 上升/下降延迟、按引脚弧延迟 | 内核接口扩展 | 需要 `write()` 知道"哪条输入边、什么方向"，比换调度器大 |
| 自己拥有网表（扁平序列）、取消剪开/接回 | L2（另立计划） | 本方案完全保留发射源码与剪开/接回 |
| 自研元件语义（全自研仿真器） | L3（另立计划） | 本方案一行元件语义都不改 |
| 接管 `sim.do` 的角色、自己推关卡输入/判定 | **红线** | 已被 12.4 的整板解释器证明不可行（[research/component-pipeline.md](research/component-pipeline.md:725)） |

## 2. 现状：三块成本，本方案只动前两块

现有调度器（[src/gate_delay.hpp](../src/gate_delay.hpp)）在**每个单位**做一次固定量的工作：

| 成本 | 代码 | 量级 |
|---|---|---|
| 全状态快照 `scratch ← STATE` | [gate_delay.hpp:688](../src/gate_delay.hpp:688)（`scratch` = 整个状态区，[:2666](../src/gate_delay.hpp:2666)） | ∝ stateSize × K / 周期 |
| `dirtyNow ← dirty` + `dirty ← 0` | [gate_delay.hpp:691](../src/gate_delay.hpp:691) 附近 | ∝ stateSize × K / 周期 |
| `applyMirrors()` 两趟 + `commitPending()` 线性扫 + tie-break 嵌套扫 | [:539](../src/gate_delay.hpp:539)、[:561](../src/gate_delay.hpp:561) | ∝ (mirrors + pending) × K / 周期 |
| 每元件每单位一次宿主问询（事件档 guard） | [:2376](../src/gate_delay.hpp:2376)（`tc_delay_dirty`） | ∝ K × 输入总数 |
| 真实求值 | 波前块 | ∝ 事件数（事件档）或 K × 元件数（分层档） |

**本方案的口径**：宿主接口（`tc_delay_begin` / `read` / `write` / `dirty` / `cycle_start` /
`units_this_pass` / `mirror` / `reset`，注册处 [native_logic.hpp:1799](../src/native_logic.hpp:1799)）
的**签名与含义不变**，生成源码的**文本形状不变**。改的是这些宿主函数内部怎么算时间、什么时候
提交、什么时候唤醒消费者。因此本方案**不新增任何游戏函数钩子、不改兼容画像**。

## 3. 语义规格

### 3.1 时基

- 内部时间 `now` 以**滴答**计：`kSubUnits = 16` 滴答 = 1 单位（`TC_GATE_DELAY_TICKS_PER_UNIT` 可覆盖）。
- `rt.time` 保留为**单位刻度**（= `now / kSubUnits`），`unitInCycle()`、`cycleStart()`、
  日志里的 `units=`、以及离线用例对 `rt.time` 的断言语义**全部不变**；新增 `rt.now`（滴答）、
  `rt.batches`、`rt.pops`、`rt.overlayBytes` 四个计数器。
- `Pending.due` 改以滴答存；延迟值由 `kindDelayTicks(kind,bits) = kindDelay(kind,bits) * kSubUnits`
  给出。周期长度 = `K * kSubUnits` 滴答，**任何 K 都是整数滴答**（K=7 的惯例真机用法不受影响）。

### 3.2 批（batch）取代单位

- 一次 `advance()` = 弹出所有 `due <= 目标时刻` 的事件并落到 STATE，形成一个**批**。
- **一次 advance 不跨周期边界**：到达 `K` 的整数倍就停下，让周期边界块（LATE、宿主/桥接块、
  `cycle += 1`、scope tick）照旧在那一次迭代里跑一次。于是 `cycle += 1` 与寄存器提交的次数、
  相位与今天完全一致。
- `unitsThisPass(site)` 的含义从"跑几个单位"变成"本 pass 允许几次批推进"（步进档返回 1，
  节流档返回 0，非 owner 仍返回 0）。生成的 `while tc_unit > 0 { tc_unit -= 1; begin() }`
  **文本不变**，只是这个循环从"K 次"变成"批次数"，而空闲电路一周期只有 1 次（周期边界那次）。
- 无输入的块（常量、关卡输入、开关）在每个周期的**第一批**运行一次即可（关卡输入本来就按
  周期变），不再每批都跑；这是把"空闲电路几乎免费"变成事实的那一条。

### 3.3 快照 = 写时复制叠加层（COW overlay）

- 批开始时不再 `memcpy` 整个状态区。每个槽在**本批第一次被写入**时，把旧字节存进 overlay；
  `read(offset,bits)` 先查 overlay，未命中**直接读 STATE**（STATE 在批开始时就是已提交值，
  所以直读是正确的）。
- overlay 用"槽 → (旧字节, 宽)"的小表 + 代数戳去重；成本 ∝ 本批变动的槽数。
- 保留全量 `scratch` 快照作为回退路径（overlay 溢出、宽度越界、诊断比对时用），
  并保留现有的 `offset + bytes <= size` 边界检查模式。

### 3.4 脏集合 = 代数戳

- 现在每单位 `memcpy` + `memset` 两个 ∝ stateSize 的数组（[:691](../src/gate_delay.hpp:691) 附近）。
  换成 `std::vector<uint32_t> stamp`（一次性分配）+ 全局 `curStamp`：`markDirty` → `stamp[o] = curStamp`；
  批结束 `++curStamp`（不 memset）；`isDirty(o)` → `stamp[o] == curStamp`（="本批变过"，与今天同义）。
- 溢出回绕时做一次全表清理（保留分支与断言，实际 2^32 批不可能触发）。

### 3.5 事件队列 = 时间轮

- 延迟有上界（当前表最大约 11 单位 = 176 滴答），所以用**环形桶**：`wheel[due % size]`，
  `size` 取 2 的幂且 ≥ 最大延迟 + 1（默认 512 滴答）。插入 O(1)、弹出 O(1)、无排序。
- 若将来引入大延迟（大容量 RAM、导线长度延迟），回退"溢出桶 + 最小堆"；计划里留接口，
  不在本期实现。
- 同一时刻的多条事件保持插入序（稳定），对称破平局的现有语义不变（见 §7 的第 1 条风险）。

### 3.6 惯性延迟（三档，默认 transport）

| 档 | 语义 | 用途 |
|---|---|---|
| `transport`（默认） | 每次求值都排一条，到期必落地 = 今天的语义 | 保持既有断言与真机读数逐字节一致 |
| `inertial` | 同一槽的未到期条目被新值取代（记住该槽最后一条未到期事件） | 短毛刺不再被拉宽到 `d` 个单位 |
| `inertial+minpulse` | 在 inertial 上再加"脉冲窄于该元件延迟则整体吞掉" | 最接近真实门的脉宽滤波 |

由 `TC_GATE_DELAY_FILTER` 选择。**默认不切**：振荡、锁存、`changed slots` 这些真机读数都是在
transport 语义下量出来的，切档属于行为变更，单独一期（M3）配独立证据。

### 3.7 周期边界与时钟

- 周期边界规则见 §3.2；`cycleStart` 判定不变。
- `publishClockPulse()` 从"每批扫全部时钟槽"改成**两个预定事件**（占空比的两个边界时刻），
  默认占空比（4 高 4 低单位）与 §32 的 trace 断言完全一致；脉宽可改成亚单位。
- 时钟槽的写回规则（§32：绘制、导线与逻辑读同一个 `clockPulse()`）不变。

### 3.8 步进

- 现有"单位步进"（`gate-delay-step.txt` + `TC_GATE_DELAY_STEP`）保留，语义不变。
- 新增**事件步进**：一次 pass 推进到下一个事件时刻。这是本方案附带的观测能力提升——
  看传播从"每单位一格"变成"每个事件一格"；`ms` 节流语义不变。由
  `gate-delay-step.txt` 的 `mode=event`（或 `TC_GATE_DELAY_STEP_MODE=event`）选择。

### 3.9 单一时间所有者

`ownsUnitClock` / `activeSite` / `perSite` / `suppressedPerSite` 的规则**保持原样**
（这是 §5.10 的真机结论）。批推进只在 owner 站点发生；非 owner 仍允许"已到达的周期边界同步一次"。

## 4. 落点（代码地图）

| 文件 | 内容 |
|---|---|
| [src/gate_delay.hpp](../src/gate_delay.hpp) | 主体：`Runtime` 加 `now`/`batches`/`pops`/overlay/代数戳/时间轮；`write()` 入轮；`commitPending()` → `advance()`；`begin()` 改批边界；`unitsThisPass()` 改批预算；`publishClockPulse()` 改预定事件；`kindDelay` 加 `* kSubUnits` 包装 |
| 同上（改写器） | 只动延迟字面量的口径（单位 → 滴答），块切分、守卫、LATE/宿主守卫、剪开/接回**都不动** |
| [tests/gate-delay.cpp](../tests/gate-delay.cpp) | 新增：调度器对拍用例、亚单位用例、惯性档用例、代数戳/overlay 边界用例；更新受影响的既有断言（§10） |
| [tests/gate-delay.ps1](../tests/gate-delay.ps1) | 不变（仍跑上面那个 exe，`-Dumps` 仍跑真机转储） |
| [tests/gate-delay-ring-playtest.ps1](../tests/gate-delay-ring-playtest.ps1) | 不变；新增一组 `TC_GATE_DELAY_SCHED=sched` 的跑法 |
| [tests/component-cost-probe.cpp](../tests/component-cost-probe.cpp) | 已有 `TC_GATE_DELAY_SAMPLE=1`（逐周期扫槽）作为行为对拍的真机侧读数；加一个耗时读数 |
| [src/loader.cpp](../src/loader.cpp) | 只加配置文件的读取（`gate-delay-step.txt` 的 `mode=`），不新增游戏钩子 |

**不新增游戏函数钩子、不新增符号别名**——本方案全部落在既有宿主函数表内部。

## 5. 分期与验收

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **M0 度量**（不改代码） | 在 ring / latch / clock / two-nand / byte-adder / legacy-270 / RV32I 副本上量：分层 vs 事件（`TC_GATE_DELAY_SCHED=event`）、K=8 vs K=4、每周期耗时与 `stateSize` | 一张基线表：每块板的 cycles/s、每 pass 耗时、固定开销占比 | 0.5 天 |
| **M1 批取代单位**（时基仍整数单位） | `advance()` + 时间轮 + 批预算 + 无输入块每周期一次；**不动**快照/脏集合实现 | **对拍**：任意整数单位的 STATE 逐字节相同（§6）；空闲电路每 pass 只剩边界工作 | 1–2 天 |
| **M2 精细时基 + COW + 代数戳** | `now`（滴答）、1/16 单位、overlay 取代全量 memcpy、代数戳取代 dirty 双数组 | 亚单位断言；内存流量计数器下降；M0 基线上重测并记录 | 2–3 天 |
| **M3 惯性延迟** | 三档过滤（默认仍 transport） | 新断言：窄脉冲被吞、短毛刺不再变宽；transport 档回归全绿 | 1–2 天 |
| **M4 事件步进 + 逐事件 trace** | `mode=event` 步进、逐事件日志 | 真机单步能看到波前；`ms` 节流照旧 | 1 天 |
| **M5 分派表**（可选，硬骨头） | 让运行时给出"本批要跑的块清单"，程序按块号跳转，去掉每批的全块问询 | 大板每周期耗时再降一档；否则停在 M2 | 2–4 天，先看 M0/M2 数据再决定 |

开关：`TC_GATE_DELAY_SCHED=layered`（今天的分层档）/ `event`（今天的事件档）/
`sched`（本方案，M1 起可用）。设置页那一行不动，默认走 `layered` 直到 M2 收口。

## 6. 对拍与验收矩阵

**对拍方法（M1 的硬门槛）**：同一块板、同一 K，旧实现与新实现在**每个整数单位**的
STATE 必须逐字节相同。两层都能做：

1. **离线（新用例，主战场）**：现有 [tests/gate-delay.cpp](../tests/gate-delay.cpp) 已经在用
   "假状态数组 + 直接调 `begin()`/`write()`/`unitsThisPass()`"的方式驱动运行时
   （见 [checkSingleTimeOwner](../tests/gate-delay.cpp:1183)），对拍用同一手法：用合成块建
   一张小网表（含常量、多驱动、环、寄存器边界、tick 源），跑 K 个单位的快照序列做逐字节比对。
2. **真机（行为侧）**：`component-cost-probe` 的 `TC_GATE_DELAY_SAMPLE=1` 逐周期扫槽，
   比对 `changed slots` 与槽序列；用现成的 playtest 命令，只加 `TC_GATE_DELAY_SCHED=sched`。

矩阵（每一项都要在旧/新两种调度下给出同一读数）：

| 场景 | 板子/用例 | 期望 |
|---|---|---|
| 空闲稳态 | two-nand 锁存（saved setting on） | `changed slots=0`，稳态 (0,1) |
| 对称破平局 | 两个 NAND 的触发器、输入 11 | 稳定到 (0,1)；**M1 必须专门覆盖**（tie-break 依赖"同批有到期条目"这个前提，见 §7） |
| 振荡 | 闭合三 NAND 环，K=7 | `changed slots=5` 的既有读数不变 |
| 门种替换 | NOR 锁存 | `gates=2 delay=1 flag=0` |
| 时钟 | 时钟 + NOT 板 | 4 高 4 低的脉冲序列与 NOT 槽反相延迟一格（§32 的 trace） |
| 混合/深板 | legacy-270 转储、byte-adder | 改写与运行不退化；括号/步进数不变 |
| 大板 | RV32I 副本 | 编译通过、推进到 cycle 1、周期耗时下降（M0 基线对比） |
| 编辑生命周期 | 新增/删除/撤销/重做 + 闭合环 | 不闪退（§5.4 的历史回归） |

## 7. 风险与未决

| 风险 | 影响 | 缓解 | 何时知道 |
|---|---|---|---|
| **对称破平局依赖"队列里有到期条目"** | 对称锁存可能不再进入破平局分支，行为偏移 | 规则本身不动，只保证"同批同时提交的条目"仍被看见；M1 对拍矩阵单列 11 输入与三 NAND 环 | M1 |
| 每批问询成本不降（直线序列每块仍一次宿主调用） | M1 的收益低于预期 | M0 先量"分层 vs 事件"，若两者接近说明问询主导；那 M1 的收益就是固定开销那部分，M5 才是硬骨头 | M0 |
| 活动率接近 100% 的电路（环振、深链） | 没得跳，只剩队列与簿记开销，可能略慢 | 接受：这类板子本来就慢；给出"不劣化超过 10%"的验收下限（ring 板） | M1/M2 |
| overlay / 代数戳的边界检查漏 | 越过状态区读写（历史上踩过越界写） | 沿用 `offset + bytes <= size` 模式；加随机宽度/边界的离线用例 | M2 |
| 批推进把周期边界跨过去 | `cycle += 1` 与 LATE 提交次数/相位错 | §3.2 的"advance 不跨周期边界"是硬规则，M1 必须跑 checkSingleTimeOwner 的扩展版 | M1 |
| 两个体互相推进（refresh/run） | 闪帧、周期翻倍（§5.10 的历史） | `activeSite` 规则不动，离线用例覆盖两个体 | M1 |
| 词法作用域（把块搬出循环 → 变量失作用域，§17.2 的历史） | 编译不过 | 本方案不改块的内联位置，只改宿主侧调度；LATE/宿主守卫位置不变 | M1 |
| 存档/撤销闪退（§5.4 的历史） | 玩家编辑时退出 | 本方案不动剪开生命周期；M1/M2 各跑一遍组件编辑 playtest | M1/M2 |
| 断言的连带修改漏掉 | 测试红/假绿 | §10 给清单 | 每期 |

## 8. 退路

四档实现对**并存**，任何一期不达标就停在上一期，不返工：

1. 默认分层档（今天）——一点不变；
2. `TC_GATE_DELAY_SCHED=event`（今天的事件档）；
3. `sched` + transport（M1/M2）——对拍已证明与 (1) 在同一整数单位上逐字节相同；
4. `sched` + inertial（M3）——行为变更，独立证据，可随时退回 transport。

若 M0 显示收益不足以支持复杂度，**只取 M1 的批推进部分**（去掉 K 倍固定成本）即可收工，
时基与语义都不用动。

## 9. 红线（延续 PLAN-gate-delay.md §8）

1. 不改 EXE、不改引擎 DLL 字节；
2. 不在普通关卡改语义；不动分数口径与通关判定；
3. 不改存档格式；开关关闭时与未装 Mod **逐字节一致**；
4. **不接管 `sim.do` 的角色**（不自己推关卡输入、不自己判定结果）——12.4 的死因；
5. 不改元件语义；不在本期改延迟数值表；
6. 不为了省事把"环"做成"自动插延迟元件"这类等价物。

## 10. 受影响断言清单（实现时别漏）

M1（批取代单位）：`checkSingleTimeOwner` 里对 `rt.time` / `rt.perSite` / `rt.pending` /
`unitsThisPass()` 返回值的断言——若 `time` 仍以单位计则**预期不变**；新增 `batches` 断言。
M1 还会牵动运行时日志字段（[gate_delay.hpp:676](../src/gate_delay.hpp:676) 的
`units= writes= commits= reads= pending=`）：保留全部旧字段，只**追加** `batches=` / `pops=` /
`overlay=`，以免 playtest 与真机日志的既有读法失效。

M2（滴答）：改写器发射的延迟字面量口径变了，`checkSample` 里
`tc_delay_write'(U64 256, U64 1, U64 (vid256), U64 1)` 这类"末参数 = 1 单位"的断言要按
滴答口径同步；`checkDelayTable` 的表值断言要改为"单位值 × kSubUnits"。
这两处是**唯一预期要改的既有离线断言**，其余（`checkTieBreak` / `checkClockPulse` /
`checkWireMirror` / `checkEventSchedule` / 剪开接回各组）应当原样通过——原样通过本身也是验收项。

## 11. 相关文档

| 文档 | 内容 |
|---|---|
| [PLAN-gate-delay.md](PLAN-gate-delay.md) | 本方案的前身：单位循环、快照、按器件延迟发布、剪开/接回、§11 现实性评估、§31 延迟表 |
| [HANDOFF-gate-delay.md](HANDOFF-gate-delay.md) | 现状、怎么跑、踩坑清单（§5.10 单一时间所有者、§5.4 编辑闪退、§32 时钟槽） |
| [research/component-pipeline.md](research/component-pipeline.md) | `get_cost` / `get_delay_cost` / `preorder` 延迟链路；§12.4 整板解释器的死因 |
| [sdk/custom-logic.md](sdk/custom-logic.md) | 原生逻辑回调的形状与相位（自定义元件的桥接边界） |
