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

游戏按 `levels.txt` 里的 schematic 名称读取 `schematics/<level>/<schematic>/circuit.data`；
只把文件写到 `schematics/<level>/circuit.data` 不会被关卡选中。加载器自身对存档路径的重定向
见 [../install.md](../install.md#独立存档与原版导入)。

## 线程与调用位置

| 接口组 | 建议调用位置 |
|---|---|
| `game`、`state`、`components`、`save` | 游戏主／渲染线程，仿真停止时 |
| `simulation.submit` | 任意线程可提交，但实际执行在仿真线程 |
| `board`、`wire` | 主／渲染线程 |
| 逻辑回调 | 游戏仿真线程（见 [custom-logic.md](custom-logic.md)） |
