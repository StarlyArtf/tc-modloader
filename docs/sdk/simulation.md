# 仿真、board、导线与存档

四组只读／低层封装，都通过 `tc_mod.h` 的 `TCMod` 聚合对象一次性加载。它们只做已核实
布局的转发，不做状态机封装。

## 仿真：`tc_simulation.h`

```cpp
int64_t cycle = mod.simulation.cycle();          // 初始可能为 -1
mod.simulation.run(model, cycle + 100);           // 提交命令 0
mod.simulation.pause(model);                      // 命令 1（refresh/stop）
mod.simulation.reset(model);                      // 命令 2（mode_reset）
int64_t setting = mod.simulation.commandSetting(0);
mod.simulation.setCommandSetting(2, 1);
```

| 成员 | 说明 |
|---|---|
| `submit(model, command, target)` | 向仿真线程提交命令（0 run / 1 refresh / 2 reset） |
| `cycle()` | 当前周期 |
| `settings`、`get_setting`、`set_setting` | 命令设置读写（含测试状态等键） |
| `inputReplay()`、`outputHistoryPins()` | 关卡输入回放与输出脚历史的只读指针（字宽关卡不一定写输出历史） |
| `keyboardCharacter()`、`keyboardCoordinate()` | 键盘输入只读指针 |

这些函数把命令交给游戏的仿真线程处理，插件不要在别的线程直接改内部状态。`model`
指针由 Hook 捕获（例如 Hook `sim_do` 时取第一个参数）或来自插件自己的探针。

## 仿真服务：`tc.simulation` V1/V2

上面那套 `TCSimulationModel` 是"自己抓符号"的老路；**服务表**才是推荐的入口，因为它把
"谁拥有这个能力、哪条线程能调、越界怎么办"写进了契约。

```cpp
#include "tc_simulation.h"

tc::simulation::Api sim{};
if (!tc::simulation::table(host, &sim)) return 2;   // 老加载器：只有 V1 读

int64_t cycle = tc::simulation::cycle(sim);

TCSimChannelV1 channels[2] = {};
channels[0].size = sizeof(TCSimChannelV1); channels[0].version = TCSIM_CHANNEL_VERSION_1;
channels[0].byte_offset = 0x40; channels[0].bits = 1;
channels[1] = channels[0]; channels[1].byte_offset = 0x48; channels[1].bits = 8;
uint64_t values[2] = {};
uint32_t stable = 0;
tc::simulation::sample(sim, channels, 2, values, &cycle, &stable);   // stable=0 时重读

tc::simulation::setSlice(sim, 1);     // 每次运行请求最多推进 1 周期
tc::simulation::runFor(sim, 64);      // 走 64 拍，每拍之间自己采样
tc::simulation::pause(sim);
```

| 入口 | 语义 |
|---|---|
| `get_state` / `read_value` | V1：周期、引擎帧、状态缓冲大小；按 `(字节偏移, 位数)` 读一个值 |
| `cycle` / `state_size` | 便捷读；未开始跑时 `cycle = -1` |
| `snapshot` | **一次调用读一批通道**，并回报这次读的周期与 `stable`（读的过程中周期没变才是 1；为 0 就该重读）。跨通道不会跨步 |
| `run_to` / `run_for` / `step` | **请求**：走游戏自己的 `sim.do`，因此和玩家按运行键是同一条路（`cycle-guard` 之类照样生效）。`run_for`/`step` 是"从现在起再走 N 拍" |
| `pause` / `reset` | 停止 / 重置的请求 |
| `set_slice` | 每次运行请求最多推进 N 个周期（0 = 不切）；配合 `run_for` 就能逐周期采样 |
| `control` | 上面这些请求的记账：`requests`、被切短的 `clamped` 次数、最近一次请求的 `target`/实际生效值/当时的周期 |

**切片语义**（`set_slice`）：这是"让调用方每周期都能拿到一个点"的最小机制，**不改生成的程序**。
切短之后运行会停在切片边界，调用方需要再发起下一次请求（`clamped` 增长就说明还没走完）；
加载器的 `sim.do` 链节在最前面处理它，所以 `cycle-guard` 之类的 Mod 看到的是已经被切短的
目标。P3 的逐周期采集器（`sim.capture`）会在生成源码里插一个 tick，届时**不需要**切片。

### 线程与调用位置

| 调用 | 可以从哪条线程调 | 说明 |
|---|---|---|
| `get_state` / `read_value` / `cycle` / `state_size` / `snapshot` | 游戏主线程（插件的 `on_frame`） | 状态缓冲由仿真线程写，`snapshot` 的 `stable` 就是给这种"边跑边读"用的 |
| `run_to` / `run_for` / `step` / `pause` / `reset` / `set_slice` | 游戏主线程 | 只是**请求**，真正的推进发生在仿真线程 |
| `control` | 任何线程 | 只读的原子计数 |
| （将来的）`sim.capture.read` | 游戏主线程 | 采集缓冲由 tick 在仿真线程写 |

### 通道寻址：`tc.sim.channel`

导线的状态偏移与位宽在**板子的导线记录**里（`+0x38` / `+0x30`）。以前每个 Mod 自己挖，
而且重编译后偏移会失效；现在有服务：

```cpp
#include "tc_sim_channel.h"

tc::sim_channel::Api wires{};
if (tc::sim_channel::table(host, &wires)) {
    TCSimWireChannelV1 channel{};
    if (tc::sim_channel::fromWire(wires, &wireHandle, &channel) == TC_SIM_CHANNEL_OK)
        tc::simulation::sampleOne(sim, channel.byte_offset, channel.bits, &value);

    // 电路改过、重编译之后：按 wire id 先找，找不到再按两端坐标找
    uint32_t live = 0;
    tc::sim_channel::resolve(wires, &boardHandle, probes, probeCount, &live);
    // 不再解析得到的通道保留 id、bits = 0（画成灰的，而不是瞎读旧偏移）
}
```

| 入口 | 语义 |
|---|---|
| `from_wire` | 板子对象快照给的**导线句柄**（同一帧内有效）→ `{byte_offset, bits, wire_id, 两端坐标}`；导线还没有状态槽时返回 `ERR_STATE` |
| `resolve` | 原地重新解析一串通道：先按 `wire_id`，再按两端坐标（重编译后记录会移动）；`resolved` 回报还活着的条数 |

## 逐周期采集：`tc.sim.capture`（P3，生成源码注入）

上面的 `tc_trace` 采样发生在渲染线程、每帧一次，快跑时会跨周期。要做到"一个周期都不漏"，
只能在**生成的程序里**取点——加载器改写编译源码（和原生逻辑桥同一套手法），在每个
`cycle += 1` 之前插一行 `game_engine.'tc_scope_tick'(U64 cycle)`。那一刻正好是"这一拍的
电路已经算完、周期号还没 +1"，所以采样点的语义是**周期 N 的稳定状态**；因此不论谁在驱动
仿真（玩家运行、单步、关卡自带测试），每一拍都会经过它。

```cpp
#include "tc_sim_capture.h"

tc::sim_capture::Api capture{};
if (tc::sim_capture::table(host, &capture)) {
    TCSimChannelV1 channels[2] = {};                 // 偏移可来自 tc.sim.channel
    channels[0].size = sizeof(TCSimChannelV1); channels[0].version = TCSIM_CHANNEL_VERSION_1;
    channels[0].byte_offset = a; channels[0].bits = 1;
    channels[1] = channels[0]; channels[1].byte_offset = b;

    TCCaptureTriggerV1 trigger{};
    trigger.size = sizeof(trigger); trigger.version = TCCAPTURE_TRIGGER_VERSION_1;
    trigger.channel = 0; trigger.edge = TCCAPTURE_EDGE_RISING;

    tc::sim_capture::configure(capture, channels, 2, 4096, &trigger);  // 4096 拍预触发窗口
    tc::sim_capture::start(capture);
    tc::simulation::runFor(sim, 200);                                  // 采集跟着运行走

    uint64_t cycles[4096] = {}; uint64_t values[4096 * 2] = {}; uint32_t rows = 4096;
    tc::sim_capture::read(capture, cycles, values, 4096, &rows);        // 最旧一行在前
}
```

| 入口 | 语义 |
|---|---|
| `configure` | 通道（同 `TCSimChannelV1`）、环形深度（1..1048576）、可选触发；会停掉正在跑的采集并重置 |
| `start` / `stop` | 开始采集（清空并从头记）／停止（保留已采集内容，之后停止增长） |
| `read` | 拷贝当前持有的行，**最旧一行在前**；`values` 按行排列（每行 `channel_count` 个值）；房间不够返回 `ERR_RANGE` 并给出能放下的行数 |
| `status` | `{channel_count, depth, rows, written, first_cycle, last_cycle, gaps, restarts, trigger_cycle, triggered, recording, injected}` |

几个**契约级**的语义，写进这里免得以后改坏：

- 环形里永远是"最近 `depth` 拍"——这就是**预触发窗口**，触发只是把触发那一行标出来，
  所以"触发点前后各若干拍"不需要额外的缓冲；
- `gaps` 统计"两次采样之间周期号不是 +1"的次数：**0 就是"一个周期都没漏"**，这是示波器
  必须能显示成 0 的数字，也是真机用例的断言；
- 周期号**倒退**（关卡被重置、新一轮运行）不算缺口：整段清掉重记，`restarts` 计数；
- 同一个周期号被**重复上报**（生成的程序里有两个注入点，一拍可能 tick 两次）既不算缺口
  也不算重启：后一次报告**改写**那一行（后报的值是稳定值）。早期版本把它当重启，结果
  每一拍都清空环形，窗口里永远只有 1 行；
- `injected` 说明当前跑着的程序里到底有没有 tick（例如关卡还没来得及重新编译）。

### 通道要用**仿真状态空间**的偏移

tick 跑在编译出来的程序里，用游戏自己的 `sim_state_read_u64(byte_offset)` 读值，所以
`configure` 的 `byte_offset` 必须是**仿真状态缓冲**里的字节偏移——`tc.sim.channel` 从
板子的导线记录里取出来的正是这个偏移，示波器应该用它。

**不要**把 `tc::trace::Sampler` 的 `inputSlot()` / `outputSlot()` 当成同一套坐标：那是
关卡 I/O **历史缓冲**（`input_replay` / `output_history_pins`）里的槽位，和状态缓冲不是
一块内存。混用不会报错，只会安静地采到全 0——真机用例
[`tests/scope-capture-playtest.ps1`](../../tests/scope-capture-playtest.ps1) 踩过一次，
所以把它写成规矩。

一个完整的用法在 [`examples/scope/plugin.cpp`](../../examples/scope/plugin.cpp)
（Mod `local.scope`）：板侧栏的示波器面板，通道全部由 `tc.sim.channel` 从板子导线解析、
数据全部来自 `tc.sim.capture`，含时基／平移／触发／双游标／VCD 导出。

## 关卡波形：`tc_trace.h`

`tc::trace::Sampler` 把关卡每个输入／输出按周期取出来，用于画波形或存 VCD。它**只读**，
写的是自己的采样缓冲，不碰仿真状态。

```cpp
#include "tc_trace.h"

static tc::trace::Sampler trace;

// tc_mod_load 里，tc::ui / TCMod 加载之后
trace.load(host, mod.simulation, mod.state);

// 每个渲染帧调用一次（关卡的 I/O 历史就在游戏自己的两份缓冲里）
trace.sample();

if (trace.ready()) {
    for (int pin = 0; pin < trace.inputCount(); ++pin)
        float latest = static_cast<float>(trace.input(trace.rows() - 1, pin));
}
trace.writeVcd("C:/path/to/trace.vcd", "level");   // 标准 VCD，1 cycle 一步
```

| 成员 | 说明 |
|---|---|
| `load(host, simulation, state)` | 校验并接管两份历史缓冲的指针；失败时保持"无效"状态 |
| `sample()` | 取一次快照（约 512 字节复制），每次都会重新解析槽位。**只在周期变化时追加一行**：暂停时不长、返回 `false`；仿真跑动时返回 `true`。每帧调用即可 |
| `ready()`／`rows()` | 是否已解析出槽位／已采样行数 |
| `inputCount()`／`outputCount()` | 当前关卡声明的输入／输出引脚数 |
| `input(row, pin)`／`output(row, pin)` | 某一行某个引脚的值（越界返回 0） |
| `cycleAt(row)`／`lastCycle()` | 该行对应的游戏周期（关卡刚加载时可能是 −1） |
| `setVisible(isInput, pin, bool)`／`visible(...)` | 勾选要监测的信号：未勾选的不画也不写进 VCD，但仍在采样 |
| `visibleInputs()`／`visibleOutputs()` | 当前勾选的信号数量 |
| `addBitProbe(label, byteOffset, width)` | 增加一条**位探针**（例如一条导线）：值 = `sim_state_read_u64(byteOffset) & ((1<<width)-1)`，与引脚并列采样／绘制／导出（VCD 变量名 `p<i>`） |
| `probeCount()`／`probeLabel(i)`／`probeValue(row, i)`／`probeOffset(i)`／`probeWidth(i)` | 探针的读取接口 |
| `setProbeVisible(i, bool)`／`probeVisible(i)`／`visibleProbes()` | 探针的显示开关 |
| `clearBitProbes()`／`canProbe()` | 清空探针／状态读取函数是否可用 |
| `inputSlot(pin)`／`outputSlot(pin)` | 解析出的槽位字节偏移，便于诊断 |
| `assumedStride()` | 是否因为某个引脚整段没变化而按间距推断槽位 |
| `writeVcd(path, scope)` | 写 VCD；没有采样时返回 `false` |

布局是实测的，不是猜测：`and_gate`（声明 2 输入 2 输出）的输入槽在字节 0 与 8、输出槽在
字节 55 与 64，值与关卡自带测试完全一致（输入 0,1,2,3；输出 0,0,0,1，见
[`tests/sim-trace-probe.cpp`](../../tests/sim-trace-probe.cpp) 与 `tests/trace.cpp`）。
采样器不硬编码这些偏移：它扫描缓冲区里"动过的字节"，再取关卡声明的数量；因此换关卡、
换引脚数都不需要改插件。需要注意两点：

- 一个引脚如果在整个采样窗口里都没有变化，就不可能被"动过"发现；采样器会按相邻槽位的
  间距把剩余引脚补齐，并通过 `assumedStride()` 明说（界面示例把它显示成
  `(slots estimated)`）。
- 字宽关卡不一定会写输出历史缓冲（见上面 `outputHistoryPins()` 的说明），这类关卡的
  输出波形可能是空的。
- 采样发生在渲染线程、每帧一次：仿真比渲染快时会跨周期（VCD 里表现为时间戳跳跃）。
  逐周期采样需要挂每周期步进函数并在仿真线程做缓冲，尚未实现。
- **行是延后一次调用落盘的**：仿真状态在步进结束时才写好，所以 `sample()` 在周期变化时
  先捕获（读关卡 I/O），下一次调用才把这一行写进去（此时读探针）。调用方不需要改代码，
  只会在关卡暂停后多出最后一行。
- 位探针的偏移从哪来：见 [research/waveform-handoff.md](../research/waveform-handoff.md)
  §3.2 —— 导线记录 `+0x38` 是状态字节偏移、`+0x30` 是位宽。**字宽网络的位宽字段尚未在
  真机核对**，目前只在 1 位网络上验证过。

## board 选择状态：`tc_board_model.h`

通过游戏自身的 `contains__modelZboardZboard_u1842` / `len__modelZboardZboard_u19087` 查询，
不手写解析 Nim HashSet。

| 成员 | 说明 |
|---|---|
| `selected_components`、`selected_wires` | 当前选中集合 |
| `prev_selected_components`、`prev_selected_wires` | 上一帧选中集合（用于检测变化） |
| `isComponentSelected(id)`、`isWireSelected(id)` | 单点查询 |
| `componentCount()`、`wireCount()` | 当前（或上一帧）选中数量 |
| 枚举接口 | 遍历选中元件／导线 ID |

## 游戏状态：`tc_game_state.h`

读取 `is_campaign`、`level_progress`、`campaign_name`、`simulation_circuit_state`、
`current_word_size` 等全局量，另提供 `levelUsedInput()` / `levelUsedOutputs()`
用于取当前关卡用到的输入／输出。示例 `examples/mod-inspector` 直接用这些字段做只读面板。

## 导线：`tc_wire_model.h`

封装导线读取、取色、添加、放置与更新，以及 `INVALID_WIRE_ID`。

```cpp
uint64_t color = mod.wire.color(wire_id);
bool placed = mod.wire.update(model, context, input, point, fifth);
```

`update` 需要真实的 board IO 上下文（通常来自 Hook `handle_update_wire...`）。
直接调用的语义与 UI 拖动一致，但**不替代** UI 自动化；鼠标命中测试仍属测试范围外。

## 存档：`tc_save_model.h`

| 成员 | 说明 |
|---|---|
| `save_count` | 保存次数（可用于检测“是否保存过”） |
| 当前 level/schematic 路径 | 通过 `__emutls_get_address` 读取当前线程的路径对象 |

**测量更正（2026-09-26，关卡生命周期用例）**：关卡自己的棋盘是
`schematics/<关卡 kind>/Default/circuit.data`——不是"关卡名"，也不是 `levels.txt` 里的那一列。
kind 写在关卡自己的 meta 里（`campaign/<关卡目录>/meta.txt` 的 `kind = …`）：关卡 "The Sandbox"
的目录是 `campaign/sandbox`、kind 是 `architecture`，所以它的棋盘在
`schematics/architecture/Default/circuit.data`。测法是把两份不同夹具分别放进两个候选目录，
游戏只加载 kind 的那一份（另一份从没被读过或写过），随后游戏自己也会把加载到的棋盘写回该路径。
加载器 `TC_COMMAND_SAVE` 因此按 kind 拼路径（`src/native.hpp` 的 `levelSchematicKind()`）；
只把文件写到 `schematics/<关卡名>/Default/circuit.data` 不会被关卡读到——这正是"退出再进后
常量回到默认值"的根因。加载器自身对存档根目录的重定向见
[../install.md](../install.md#独立存档与原版导入)。
完整测法与证据在 [../research/schematic-paths.md](../research/schematic-paths.md)。

## 线程与调用位置

| 接口组 | 建议调用位置 |
|---|---|
| `game`、`state`、`components`、`save` | 游戏主／渲染线程，仿真停止时 |
| `simulation.submit` | 任意线程可提交，但实际执行在仿真线程 |
| `board`、`wire` | 主／渲染线程 |
| 逻辑回调 | 游戏仿真线程（见 [custom-logic.md](custom-logic.md)） |
