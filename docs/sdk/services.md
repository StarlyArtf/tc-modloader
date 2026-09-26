# 版本化服务

`TCHost` 保留进程级基础设施；新的游戏语义 API 通过 `query_service` 获取独立版本的服务表，
避免每增加一种游戏能力就继续扩大整个宿主结构。

## 查询

```cpp
#include "tc_mod_api.h"

TCBoardApiV1 board{};
int status = tc::boardService(host, &board);
if (status != TC_SERVICE_OK) return 2;
```

服务 id 与版本是精确契约：未知 id 返回 `TC_SERVICE_ERR_UNAVAILABLE`，已知服务但版本不支持
返回 `TC_SERVICE_ERR_VERSION`，输出结构太小返回 `TC_SERVICE_ERR_SIZE`。Loader 不会把另一个
布局“尽量”写进调用方缓冲区。

每张表以 `size`、`version` 开头，随后是该服务自己的 `context` 和函数指针。函数必须使用表中
的 context，不能换成 `TCHost.context`。服务表可以按值保存到进程结束；它包含的游戏对象句柄
仍服从各自生命周期。

## Board V1

`TC_SERVICE_BOARD` / `TCBoardApiV1` 是第一张服务表，提供：

- `get_current`：取得当前 Board 句柄；主菜单返回 `TC_HANDLE_ERR_UNAVAILABLE`；
- `validate`：1 有效、0 已失效、负数为调用错误；
- `resolve`：兼容迁移用的临时底层指针，只对有效句柄成功。

```cpp
TCBoardApiV1 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCGameHandle handle{};
if (board.get_current(board.context, &handle) == TC_HANDLE_OK) {
    if (board.validate(board.context, &handle) == 1) {
        // 一般 Mod 不应长期使用 resolve；V2 快照直接接受 handle。
    }
}
```

`TCHost` 上既有的三个句柄入口仍然保留，供已经编译的 0.6.0 Mod 使用；新代码优先查询 Board
服务。

## Board V2：同帧只读快照

V2 保留完整 V1 前缀，并增加 `capture_snapshot`。`TCBoardSnapshotV1` 是调用方拥有的固定值结构，
不含游戏裸指针、字符串或容器视图；一次调用读取 Board 身份、引擎帧、仿真周期，以及当前和
上一份选择中的组件/导线数量。

```cpp
TCBoardApiV2 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCGameHandle handle{};
TCBoardSnapshotV1 snapshot{};
if (board.get_current(board.context, &handle) == TC_HANDLE_OK &&
    tc::captureBoardSnapshot(&board, &handle, &snapshot) == TC_SNAPSHOT_OK) {
    if (snapshot.flags & TC_BOARD_SNAPSHOT_HAS_SELECTION) {
        // snapshot.selected_component_count / selected_wire_count
    }
}
```

一致性约束：

- 只能在 Loader 调用 Mod 的游戏主/渲染线程上捕获；其他线程返回 `TC_SNAPSHOT_ERR_THREAD`；
- 读取前后都会验证 Board 句柄与引擎帧，重入导致变化时返回 `TC_SNAPSHOT_ERR_RETRY`，不会发布
  拼接了两个场景或两帧的数据；
- 失效句柄返回 `TC_SNAPSHOT_ERR_STALE`；缓冲区不足返回 `TC_SNAPSHOT_ERR_SIZE`；
- `flags` 决定对应字段是否可用。字段为 0 但 flag 未设置，含义是“数据源不可用”，不是“数量为 0”。

快照可以在调用后按值保存；其中嵌入的 `board` 句柄若用于后续调用，仍须重新验证。

## Board V3：Component/Wire 句柄

V3 增加 `capture_objects`，从 Loader 自己验证过的 Board 序列签发 Component/Wire 句柄。调用方
提供缓冲区；第一次用零容量查询所需数量，收到 `TC_SNAPSHOT_ERR_CAPACITY` 后分配并立即重试：

```cpp
TCBoardApiV3 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCBoardObjectSnapshotV1 objects{};
TCBoardObjectBuffersV1 buffers{
    sizeof(buffers), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
    nullptr, 0, nullptr, 0
};
int status = tc::captureBoardObjects(&board, &handle, &objects, &buffers);
if (status == TC_SNAPSHOT_ERR_CAPACITY) {
    std::vector<TCGameHandle> components(objects.component_count);
    std::vector<TCGameHandle> wires(objects.wire_count);
    buffers.components = components.data();
    buffers.component_capacity = components.size();
    buffers.wires = wires.data();
    buffers.wire_capacity = wires.size();
    status = tc::captureBoardObjects(&board, &handle, &objects, &buffers);
    // status == TC_SNAPSHOT_OK 时，written 个句柄都来自这一次同帧枚举。
}
```

这些子句柄比 Board 句柄更短命：**只在签发它们的引擎帧内有效**。下一帧开始、Board 离开或
换关都会返回 stale；调用另一个对象快照不会让同帧中先前签发的句柄失效。这样即使游戏的 Nim
序列因编辑而在帧间搬迁，也不会把旧地址误认成新对象。`get_current` 仍只接受 Board kind，
所以 Mod 不能用任意 ID 或裸指针伪造可信子句柄。

容量不足时 Loader 只填写 `component_count` / `wire_count`，`written` 保持 0，不发布半份对象集。
成功时 `component_written == component_count`、`wire_written == wire_count`。快照与缓冲区都属于
调用方；其中没有 Loader 借出的数组。

两条真机实测（`tests/game-handle-probe-playtest.ps1`，原始行在
[验证体系](../verification.md)）值得单列，因为它们决定了 Mod 该怎么用这份枚举：

- **kind 为 0 的全零记录是"死槽"，不是元件**：游戏自己的 kind 表里没有 0（`0x03`–`0x0b` 是逻辑
  门、`0x3c`／`0x44` 是关卡输入／输出引脚、`0x4e` 是自定义实例）。板子加载时它出现在序列第 0 条，
  **撤销之后也会出现**——被撤掉的元件不会让序列缩短，而是留在原地变成这样一条墓碑（实测见
  [commands.md](commands.md)）。枚举和读取都照实返回它，**数元件必须跳过 `kind == 0`**：
  `TCBoardObjectSnapshotV1.component_count` 是序列长度，不是活元件数。
- **`level.load` 那一刻的棋盘可能还没稳定**：同一关在加载帧内读到 3 元件／1 导线，而相邻帧是
  1 元件／0 导线。需要"关卡已经就绪"的状态时，应当在之后的帧重新枚举一次，而不是长期缓存加载
  回调里那一份。

## Board V4：对象数据读取

V4 在 V3 的对象枚举之上加两条按句柄读取的入口：

```cpp
TCBoardApiV4 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCComponentInfoV1 component{};
if (tc::readComponent(&board, &component_handle, &component) == TC_SNAPSHOT_OK) {
    // component.kind / component.x / component.y / component.rotation / component.id
}
TCWireInfoV1 wire{};
if (tc::readWire(&board, &wire_handle, &wire) == TC_SNAPSHOT_OK) {
    // wire.id / wire.x / wire.y / wire.bit_width / wire.state_byte_offset
}
```

每个字段都取自指定构建自己的记录，并且逐一有出处；语义还没核实的字段不进结构，而不是先起个名字
再猜。

| 结构 | 字段 | 记录位置 | 证据 |
|---|---|---|---|
| `TCComponentInfoV1` | `kind` | `+0x00` | `tests/component-placement-probe.cpp` 写回同一字节并找回元件 |
| | `x` / `y` | `+0x02` / `+0x04`（i16） | 同上：放置模板写入 `(y<<16)\|x`，探针按 i16 读回后命中 |
| | `rotation` | `+0x06` | 菜单放置模板的方向字节（取值未枚举） |
| | `id` | `+0x08` | 游戏自己的元件身份：代价求和与实例链都按它查找 |
| | `custom_prototype_id` | `+0x188` | 自定义实例引用的原型 id；`kind == 0x4e` 时才置 `HAS_CUSTOM_PROTOTYPE` |
| `TCWireInfoV1` | `id` | 序列下标 | 游戏自己的 `get_wire` 返回的就是这个下标（见 [research §3.2](../research/waveform-handoff.md)） |
| | `x1` / `y1` / `x2` / `y2` | `+0x18`／`+0x1a`／`+0x1c`／`+0x1e`（各 i16） | 导线的**两个端点**；战役关卡实测 `(9,0)->(-9,0)`，正好落在输入引脚与输出引脚之间 |
| | `bit_width` | `+0x30` | 同一节；**目前只在 1 位网络上逐项核对过**，所以超出 1–64 只报"不可用" |
| | `state_byte_offset` | `+0x38` | 它在仿真状态缓冲里的字节偏移，配合 `sim.state.read` 取这条线的值 |

导线记录早期被读成"一个端点的 x/y"，`add_wire_from_pos` 的实验推翻了它：`+0x18` 起是**两对**
int16，x1/y1 与 x2/y2。已修的读法与证据见
[research/board-object-fields.md](../research/board-object-fields.md) §3。

刻意**没有**暴露的字段是元件记录里的实例父链（`+0x10`／`+0x18`）。原因不是懒：两次真机 dump
显示板级记录在这两个偏移上是**空的**（连带名字的关卡引脚也是零），读它们的代码都跑在编译期
扁平序列上。也就是说它是扁平化时写进副本的信息，不属于板块；要暴露得另开一个编译图服务。
证据与另一处收口（引脚描述符锚点）见
[research/board-object-fields.md](../research/board-object-fields.md)。

引脚几何走 `sdk/tc_game_model.h` 的 `tc::prototypeInputPinPoint()` / `...PinWordSize()`：由
元件自身的 `custom_prototype_id`（或内置 kind）取出原型，就能拿到每个引脚相对元件原点的坐标与
位宽——把 `TCComponentInfoV1` 和导线端点放在一起，就能自己算出连接关系。

一致性约束与 V2／V3 相同（主线程、同帧、读取前后复核），另加一条：**记录指针必须仍然落在当前对象
序列的元素位置上**。所以枚举之后棋盘被编辑过、或者句柄已经跨帧，都返回
`TC_SNAPSHOT_ERR_STALE`，而不是去读一个可能已经还给游戏的内存地址。

错误码沿用快照家族：`TC_SNAPSHOT_ERR_ARGUMENT`（空指针、缓冲区不足，或传入了另一个 kind 的
句柄）、`TC_SNAPSHOT_ERR_STALE`、`TC_SNAPSHOT_ERR_THREAD`、`TC_SNAPSHOT_ERR_UNAVAILABLE`
（对象表读不到）、`TC_SNAPSHOT_ERR_RETRY`（读取期间棋盘发生变化）。

占位记录（`kind == 0`）**不是错误**：它照实返回，由调用方跳过；把它当成"读取失败"会让 Mod 在
每个空板上都报错。

## Board V5：元件引脚

V5 把引脚也变成"给一个句柄就能读"的数据：宿主用元件自己的 kind（自定义实例则用
`custom_prototype_id`）取出原型快照，把引脚列表复制到调用方缓冲区，然后立刻释放原型——
**没有任何借出的原型指针会离开这次调用**。

```cpp
TCBoardApiV5 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCComponentPinsV1 pins{};
TCComponentPinBuffersV1 countOnly{sizeof(countOnly), TC_COMPONENT_PINS_VERSION_1, 0, 0, nullptr, 0};
int status = tc::readComponentPins(&board, &component_handle, &pins, &countOnly);
if (status == TC_SNAPSHOT_ERR_CAPACITY) {          // 第一次只取数量
    std::vector<TCPinInfoV1> storage(pins.input_count + pins.output_count);
    TCComponentPinBuffersV1 buffers{sizeof(buffers), TC_COMPONENT_PINS_VERSION_1, 0, 0,
                                    storage.data(), storage.size()};
    status = tc::readComponentPins(&board, &component_handle, &pins, &buffers);
    // status == TC_SNAPSHOT_OK 时，storage 的前 pin_written 项就是全部引脚
}
```

引脚顺序固定为**先输入后输出**；`TCPinInfoV1.x`／`y` 是相对元件原点的原理图坐标，
`bits` 是声明的位宽，`word_size_raw` 是原型里的原始值。原型用 `AUTO_SIZE` 表示"由连接的网络
决定"时置 `TC_PIN_INFO_WIDTH_AUTO`、`bits` 为 0。

实测值来自 `tests/game-handle-probe-playtest.ps1`（战役关卡，一次真实运行）：

```text
Board pins: prototype reads armed, 125 built-in kinds
PROBE: pins 1 kind=0x3c in=0 out=1 p0=o(1,0,w1)
PROBE: pins 2 kind=0x44 in=1 out=0 p0=i(-1,0,w1)
PROBE: component pins status=0 read=3 of=3 pins=2 expected=2 zero=1 auto=0
```

这两行与 `build/kinds.txt` 里同一构建的 `0x3c "Input"`／`0x44 "Output"` 条目逐字段相同，所以
它同时验证了原型查找、描述符锚点（`TCPin* + 8`）与字宽字段。

自定义实例走的是另一条分支（`get_custom_prototype`），由 `component-placement-playtest` 在真机上
覆盖——同一个沙箱里放一个 fixture AND2，定义是"两个 1 位输入 + 一个 1 位输出"：

```text
component placement custom pins status=-7 inputs=2 outputs=1 kind=0x4e
component placement pins p0=i(-1,-1,w1) p1=i(-1,0,w1) p2=o(2,-1,w1) written=3
```

报出的偏移是游戏把定义里的引脚节点归一化之后的值（定义自己写的是 `(-11,-13)`／`(-11,-7)`／
`(14,-10)`），所以 Mod 直接拿这里的坐标即可，不要再去读定义。

边界：

- 主线程、同帧、句柄与记录必须仍属于当前棋盘（与 V2–V4 相同）；
- 容量不足只填 `input_count`／`output_count` 并返回 `TC_SNAPSHOT_ERR_CAPACITY`，不写半个列表；
- 内置 kind 只用**启动时枚举过的集合**里的键，绝不把未知 kind 交给游戏（未知键会让游戏抛
  Nim 错误，插件无法恢复）；不在集合里的 kind 返回 `TC_SNAPSHOT_ERR_UNAVAILABLE`；
- `kind == 0` 的占位记录在游戏的 `PROTOTYPES` 表里是**空原型**，因此它解析为"0 个引脚"而不是
  错误（真机日志里的 `zero=1`）。Mod 仍应按 kind 跳过它，见上文 V3 一节；
- 自定义实例的原型缺失时游戏返回空原型，也同样读成 0 个引脚；要区分"没注册"请配合
  `hasCustomPrototype(id)`。

## Board V6：导线两端连到哪个引脚

游戏按**几何**连通：引脚的网格位置 = 元件位置 + 引脚相对偏移（`read_component_pins` 报的那个），
导线端点落在这个点上就是玩家看到的那条连接。V6 用一个调用把这件事回答掉：

```cpp
TCBoardApiV6 board{};
if (tc::boardService(host, &board) != TC_SERVICE_OK) return 2;

TCWireEndsV1 ends{};
if (tc::readWireEnds(&board, &wire_handle, &ends) == TC_SNAPSHOT_OK) {
    for (int i = 0; i < 2; ++i) {
        if (!(ends.ends[i].flags & TC_WIRE_END_HAS_COMPONENT)) continue;  // 悬空端
        // ends.ends[i].component / .direction / .pin_index / .x / .y
    }
}
```

实现只比较**邻近窗口**内的元件（引脚偏移在实测里不超过 ±20 格），所以一次查询不会为整块板克隆原型；
窗口内先命中的引脚即为结果（同一点上不会有两个引脚，实测未遇到）。

真机证据（`game-handle-probe`，战役关卡那条线）：

```text
PROBE: board v6=6 prefix=1
PROBE: wire ends status=0 end0=(9,0,dir0,pin0,kind=0x44) end1=(-9,0,dir1,pin0,kind=0x3c)
```

`(9,0)` 落在 0x44（Output 元件，位于 (10,0)，输入端口 `(-1,0)`）上，`(-9,0)` 落在 0x3c（Input
元件，位于 (-10,0)，输出端口 `(1,0)`）上——两端都解析成了**元件句柄**，并用 V4 回读确认了 kind。
`pin_index` 的方向内序号与 `read_component_pins` 的顺序一致（输入在前、输出在后）。

## tc.simulation：仿真读数

板上的值住在游戏自己的仿真状态缓冲里。`TC_SERVICE_SIMULATION` 把"读一个槽位"和"问当前周期"
这两件事变成服务调用，Mod 用 V4 拿到的导线 `state_byte_offset` + `bit_width` 就能读它的值：

```cpp
TCSimulationApiV1 sim{};
if (tc::simulationService(host, &sim) != TC_SERVICE_OK) return 2;

TCSimulationStateV1 state{};
tc::simulationState(&sim, &state);        // cycle / engine_frame / 状态缓冲大小

uint64_t value = 0;
tc::readSimulationValue(&sim, wire.state_byte_offset, wire.bit_width, &value);
```

语义与游戏自己的 `sim_state_read_bits(offset, width)` 相同：读一个 64 位字，保留低 `width` 位。

两条必须记住的规则：

- **时机**：状态是仿真步进**结束时**写的。在"周期刚变化"的那一帧读，可能还是上一周期的值——
  这是波形面板当初踩过的坑，服务沿用同一条规则（见
  [research/waveform-handoff.md](../research/waveform-handoff.md) §2.3b）。
- **边界**：偏移必须落在模拟器初始化时分配的 10,240,000 字节状态缓冲内，越界返回
  `TC_SIMULATION_ERR_RANGE`，不会变成一次野读；`bits` 只接受 1–64。

只能读，不能写：V1 没有"设置某个槽位"的入口，改游戏状态走命令总线与事务。

真机证据（`game-handle-probe`，一次真实运行）：

```text
Simulation reads: armed, state buffer 10240000 bytes
PROBE: sim state status=0 cycle=-1 frame=134 state-size=10240000 flags=7
PROBE: sim value status=0 raw-status=0 slot=256 width=1 value=0 raw=0 masked=0 agree=1
PROBE: sim state status=0 cycle=-1 frame=428 state-size=10240000 flags=7
```

`flags=7` 表示周期、引擎帧与状态缓冲三项都可用；探针在这两个关卡里没有启动仿真（`cycle=-1`），
所以值是 0——**"值随周期变化时的正确性"由波形用例负责**（它跑的正是同一个读取函数，并把探针值
与关卡自带的输出历史逐行比较，见 [验证体系](../verification.md)）。本服务这一层验证的是：
别名已武装、偏移在界内、掩码规则与游戏一致。

## tc.io_value：游戏自己的数值编辑器用的那一层

关卡与元件的**全局输入**、以及常量元件的**数值框**，都是游戏自己的对象。`TC_SERVICE_IO_VALUE`
把它们读、写、按位翻转、求值和格式化收成一张表，Mod 不必再猜游戏怎么解析表达式、怎么按位宽
截断、怎么写回仿真：

```cpp
#include "tc_io_value.h"

TCIoValueApiV1 io{};
if (!tc::io_value::table(host, &io)) return 2;      // 老 Loader：走 Mod 自己的兜底

uint64_t mask = 0;
tc::io_value::evaluate(io, "0xFFFFFFFF^(1<<23)", &mask);   // 玩家在原生框里也是这么写

uint32_t width = 0;
tc::io_value::inputWidth(io, board, pinIndex, &width);      // 引脚声明的位宽
tc::io_value::writeInput(io, board, pinIndex, value & ~mask);
```

表里每一项的分工：

| 入口 | 语义 |
|---|---|
| `evaluate` | 解析一个表达式；语法是游戏数值框接受的那一套（十进制、`0x`/`0b`/`0o`、`~`、一元 `-`、`* / %`、`+ -`、`<< >>`、`& ^ \|`、括号、数字间 `_`）。 |
| `format_value` | 按 `TC_IO_VALUE_FORMAT_*`（二进制/十六进制/无符号/有符号）格式化，写进调用方缓冲区；缓冲区不够时截断并在 NUL 处结束，同时返回 `TC_IO_VALUE_ERR_SIZE`。 |
| `read_input` / `write_input` | 按**板上的元件下标**读写全局输入；`write_input` 与原生数值框一样先按位宽截断。 |
| `flip_input` | 原生"点击一个位方块"的路径（含游戏的周期重置语义）；`bit` 必须小于位宽。 |
| `input_width` | 元件记录里声明的位宽（1–64），读不到时回退 8。 |
| `write_constant` | 常量元件的完整写入口：写设置值 + 更新运行时值槽 + 刷新正在跑的仿真，**不触发整板重编译**。 |
| `write_constant_slot` | 只更新运行时值槽（高级用法：调用方自己负责设置值与刷新）。 |

必须记住的规则：

- **线程**：除 `write_constant_slot`（内部只有一把互斥锁）以外，全部要在游戏的**主/渲染线程**
  调用，也就是 Mod 画界面、处理点击的地方；逻辑元件的线程里调用返回
  `TC_IO_VALUE_ERR_THREAD`。
- **时点**：`read_input` 读的是仿真输入回放里的当前值，语义与 `get_component_global_input` 一致；
  `write_input` 走原生写值路径，因此和玩家手打一样会触发游戏的周期语义。
- **宽度**：所有写值都按引脚位宽截断；`>64` 位当前不支持（位宽上限就是 64）。
- **表达式求值**：V1 用 Loader 自己的解析器，**没有**调用游戏内部的 `evaluate`。原因是它接收
  Nim 字符串并按引用传参、表达式非法时靠 Nim 的错误标志上报，而 Loader 里的异常守卫无法兜住
  这条路径（见 [fault_guard 的测量](../verification.md)）——所以要先在探针构建上确认调用形状，
  再把"优先用游戏求值器"接上；在那之前，解析器覆盖的语法与数值框一致，行为不受影响。

V1 的离线证据是 `tests/io-value-service.ps1`（fast 层）：解析器的一份断言表（含
`0xFFFFFFFF^(1<<23)`、`~(1<<23)`、`(0xF<<8)|0x3`、`1<<64` 归零、`1/0` 报错、字面量超过 64 位
报 `RANGE`）、位宽截断、以及服务表形状（`context` + 八个入口）。真机端到端（在真关卡里
`evaluate` 后写值、再回读）随第一个使用本服务的 Mod（打孔纸带的掩码功能）一起验收。

## tc.pin_order：左侧 IO 面板的引脚顺序

游戏左侧的"输入状态/输出状态"面板并不是每次读电路板：`build_io_state_view` 画的是 presenter
context 里缓存下来的三段序列（输入、输出，以及工坊用的内存/寄存器一组），**这些元素的前后
次序就是玩家看到的次序**。`TC_SERVICE_PIN_ORDER` 就是这一层的通用接口：描述面板现在在显示
什么，并把"顺序"交给 Mod。

```cpp
#include "tc_pin_order.h"

TCPinOrderApiV1 pins{};
if (!tc::pin_order::table(host, &pins)) return 2;            // 老 Loader：面板保持原样

TCPinOrderEntryV1 row{};
tc::pin_order::entry(pins, TC_PIN_ORDER_GROUP_INPUTS, 0, &row);   // row.key / row.width / row.name
tc::pin_order::move(pins, TC_PIN_ORDER_GROUP_INPUTS, 0, 2);       // 把第 0 个放到第 2 个位置
```

表里每一项的分工：

| 入口 | 语义 |
|---|---|
| `count` | 这一组面板现在有几个条目（面板不在屏幕上时为 0）。 |
| `entry` | 按**当前位置**取一个条目：`key`（元件下标，条目的身份）、`width`（位宽）、`name`。 |
| `move` | 把 `from` 位置的条目挪到 `to` 位置，两者都是"现在显示的顺序"里的下标；之间的条目依次让位。 |
| `order` | 面板下一帧会画出的 key 序列（缓冲区太小时返回 `ERR_RANGE`，但 `count` 仍是完整长度）。 |
| `set_order` | 一次装进整份顺序；面板没有的 key 会被忽略，未提到的条目按面板自己的相对次序跟在后面。 |
| `reset` | 丢掉 Mod 的顺序，回到面板自己构建的次序。 |

V2 在完全保留上述 V1 前缀后，增加条目布局的两个入口：

| 入口 | 语义 |
|---|---|
| `include_bounds` | 内部控件按 `{frame, group, key}` 向 Loader 汇报一块真实的屏幕坐标矩形；同帧、同条目的多个矩形自动取并集。 |
| `bounds` | 外层装饰读取该并集；不存在时返回 `TC_PIN_ORDER_ERR_NOT_FOUND`。Loader 保留最近三个布局帧，因此晚一帧绘制的装饰仍能读取完整结果。 |

这个接口解决的是**容器与内容的所有权**，不是另一套排版：比如打孔纸带负责汇报它在输入条目里
实际画出的纸带、掩码和数值框范围，引脚顺序 Mod 只负责让外框包含该范围。双方都不再根据另一方
的固定高度或“下一条在哪里”猜测；最后一条即使没有下一条锚点也能正确贴合。老 Loader / 老 Mod
仍可查询 V1，排序 ABI 不变。

必须记住的规则：

- **身份用 key，不用位置**：顺序存的是 key 序列（元件下标），所以重命名引脚触发的缓存重建
  （`reload_custom_prototype` 之后 Loader 会重建同一份缓存）会**重新套用**这份顺序；而换成
  另一组引脚时（换了关卡）这份顺序会自动失效，不会被错误地应用。
- **改动在下一帧生效**：`move`/`set_order` 只记录目标顺序，Loader 在 `build_io_state_view`
  的入口、也就是面板读缓存**之前**才真正重排。面板正在画的那一帧不会被就地改动。
- **只动缓存，不动电路**：重排是整条记录（32 字节）的置换，元件下标跟着记录一起走，所以数值
  输入框照常可用，也不需要重编译；字符串的所有权不变，缓存重建时每条名字仍然只被释放一次。
- **线程与时点**：和 Board / Simulation 一样，只在该面板被构建、绘制的主/渲染线程上调用；
  面板不在屏幕上（不在关卡或元件工坊里）时返回 `TC_PIN_ORDER_ERR_STATE`。
- **布局是被钉住的**：Loader 侧读的记录布局（descriptor `{count, payload}`、payload 块开头的
  一个字、32 字节的 `{元件下标, 位宽, 名字{长度, 数据}}`）是实测出来的，
  `TC_MODLOADER_PIN_ORDER_LOG=dump` 会把当前进程里的原始结构打进 loader.log，供新版本游戏
  重新对齐；对不上时 Loader 选择不动作，面板保持原样。
  **描述符里的 `count` 才是条目数**（面板就是按它画的）；块开头那个字是**这块的容量**，列表
  一次性分配好（关卡面板）时两者相等，而面板一条条长出来时（元件工坊、手动加输入器件的板面）
  容量按 1、2、4、8、16 翻倍，所以条数不是 2 的幂时两者本来就**不相等**。早期版本要求它们
  相等，于是"3 个引脚整组手柄消失、4 个又回来、8 个又回来"——现在的校验只要求
  `容量 ≥ 条数`，并把容量当上界使用。

离线证据是 `tests/pin-order.ps1`（fast 层）：用与游戏同形状的假缓存验证 `count`/`entry`/`move`/
`order`/`set_order`/`reset`，以及"同一组引脚重建后顺序保留、换一组引脚后顺序失效"这两条规则；
同一用例还验证多个内部控件的矩形会取并集、上一完成帧可读、过期帧会清理。
真机端到端是 `tests/pin-order-playtest.ps1`（game 层）：`-Probe` 打印面板的锚点与 `tc.pin_order`
的回答，`-Drag` 用真实鼠标消息拖一次并回读顺序，`-Dump` 让 Loader 打印原始记录。

## tc.component.registry：这次会话注册了哪些元件类型

自定义元件的注册入口一直是 `register_logic`（导入一份定义、把它的输出驱动换成回调）与
`register_component`（声明引脚，由加载器生成脚手架）。`TC_SERVICE_COMPONENT_REGISTRY` 是它们
的**只读目录**：这次会话登记了哪些类型、每个类型什么形状、以及**注册被拒绝时的原因**。

```cpp
#include "tc_component_registry.h"

TCComponentRegistryApiV1 registry{};
if (!tc::component_registry::table(host, &registry)) return 2;   // 老 Loader：跳过这一节

for (uint32_t i = 0; i < tc::component_registry::count(registry); ++i) {
    TCComponentTypeInfoV1 type = tc::component_registry::typeInfo();
    if (tc::component_registry::get(registry, i, &type) != TC_COMPONENT_REGISTRY_OK) continue;
    if (!type.active) log(std::string(type.name) + " refused: " + type.status);
    TCComponentPinInfoV1 pin = tc::component_registry::pinInfo();
    if (tc::component_registry::pin(registry, type.custom_id, TC_COMPONENT_PIN_INPUT, 0,
                                    &pin) == TC_COMPONENT_REGISTRY_OK)
        log(std::string(pin.name) + " = " + std::to_string(pin.bits) + " bits");
}
```

表里每一项的分工：

| 入口 | 语义 |
|---|---|
| `count` | 会话内已登记的类型数（**包含被拒绝的**，它们也在目录里）。 |
| `get` | 按目录位置取一条（注册顺序）。 |
| `find` | 按 `custom_id`（游戏侧的身份）取一条。 |
| `pin` | 取某个类型某个方向的一根引脚：`pin_id`、位宽、名字。 |

字段与规则：

- **身份**：`custom_id` 是游戏自己用的身份；`type_id` 是加载器给日志/界面用的 Mod 命名空间拼写
  `"<owner mod>/0x<custom id>"`。计划里"由 Mod 声明的字符串 type_id"属于类型定义 V2 那一阶段。
- **引脚**：`pin_id` 是 `(direction << 32) | index`，与 `TCNativeComponentDefinition` 的引脚数组
  顺序一致；`pin_limit` 说明每个方向最多报告多少根（当前 32）。
- **名字与形状**：声明式注册登记的引脚名会被保留；桥接时从游戏原型读到的**位宽**会覆盖声明值，
  所以 `bits` 反映的是游戏真正编译出来的形状。
- **被拒绝的类型**：`active=0` 且 `status` 写明原因；**第一个原因不会被后续更笼统的原因覆盖**
  （桥接那句"最多每方向八脚、每脚 1–64 位、总输入 ≤128 位"才是有用的那句）。同一个 id 之后注册
  成功会替换掉这条记录。
- **拒绝的 Mod 不留痕**：插件被拒绝时它的类型从目录里删除，和它的 Hook、界面一致。
- **能力位**：目前只报告加载器能自行判断的三项——`LOGIC`（回调已接入游戏）、`WIDE_PIN`（有引脚
  超过 1 位）、`MULTI_PIN`（某一方向不止一根脚）。计划里其余能力（STATE/RENDER/INPUT/STORAGE/
  ALIAS）不能从一次回调注册推出来，等类型声明阶段由类型显式声明。
- **线程**：注册发生在 Mod 加载时；查询可以在帧回调里随便读，看到的是这一帧之前已加载的类型。

离线证据是 `tests/component-registry.ps1`（fast 层，`tests/component-registry.cpp`）；真机证据是
`tests/component-registry-playtest.ps1`（game 层）：它让一个探针 Mod 注册一个必然被拒绝的定义，
再打印整个目录，断言被拒绝项的 `status` 与 `example.byte-adder` 那个已桥接类型的引脚名/位宽/代价。
用法示例见 `examples/mod-inspector` 的 "component types" 一节。

## tc.component.types：注册一个 V2 元件定义

`host->register_component`（V1）的回调结构体 `TCLogicIO` 里是固定的 `inputs[8]` /
`outputs[8]` / `state[8]`，所以"超过八脚"和"自己决定状态大小"这两件事它表达不了。
`tc.component.types` V1 就是给这两件事的注册入口，SDK 封装在 `sdk/tc_component_types.h`：

```cpp
#include "tc_component_types.h"

static void logic(TCLogicIOV2* io) {          // 借用数组：只在本次回调期间有效
    if (io->phase == TC_LOGIC_RESET) return;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < io->input_count; ++i) acc ^= io->inputs[i];
    io->outputs[0] = acc & 1u;
    if (io->state && io->state_words) io->state[0] += 1;   // 只有 CYCLE 会提交
}

tc::component_types::Api types{};
if (tc::component_types::table(host, &types)) {
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = 0x4d5949445f303031ULL;   // 存档里的稳定标识
    definition.type_id = "my-mod/xor9";             // 可选，供查询/未来编辑器
    definition.name = "Xor 9";
    definition.inputs = ins;  definition.input_count = 9;
    definition.outputs = outs; definition.output_count = 1;
    definition.state_words = 2;                    // 每实例持久字数量，0 = 无状态
    static const uint8_t defaults[] = {1, 0, 0, 0};
    definition.config_schema = 1;
    definition.config_size = sizeof(defaults);
    definition.default_config = defaults;          // 宿主在注册时复制
    definition.gate_cost = 1; definition.delay = 1;
    definition.callback = &logic;
    tc::component_types::registerDefinition(types, &definition);
}
```

表本身按 Mod 发放（和命令总线一样）：`register_definition` 需要调用方的 mod id 与数据目录，
这正是 V1 入口用 `p.host` 的那两个字段。返回 `TC_COMPONENT_TYPES_*`：
`OK` / `ERR_ARGUMENT`（定义不合法）/ `ERR_DUPLICATE`（id 已占用）/
`ERR_UNSUPPORTED`（形状桥接不了）/ `ERR_GAME`（游戏拒绝导入）/ `ERR_BUDGET`（输入位宽超预算）。
被拒绝的定义同样进 `tc.component.registry` 目录，带自己的原因，和 V1 一致。

## tc.component.geometry：声明类型 footprint 与逐实例交互范围

`tc.component.geometry` V1 让一个 Mod 在 `tc_mod_load` 中、注册类型之后，为自己拥有的
`custom_id` 声明棋盘矩形。当前游戏构建唯一已验证可安全写入的是局部矩形序列，因此 V1 有意只
提供一个 `set_footprint`；没有证据支持的独立 `set_hit_box` 不进入 ABI。占位/放置路径已经真机
确认；真人手测已确认**可拖区域就是 footprint 矩形**，包括游戏没有画任何像素的空白角落；带脚和
完全无脚的 12×6 类型都通过。因此无需再增加同义的 `set_hit_box`。旧的相反读数来自命令放置后
漏登记、选中集闩锁与拖动碰撞干扰，完整证据见
[research/component-hitbox-path.md](../research/component-hitbox-path.md)。

```cpp
#include "tc_component_geometry.h"

tc::component_geometry::Api geometry{};
if (tc::component_geometry::table(host, &geometry)) {
    // 以元件锚点为中心，占 12 × 6 个板面格。
    int status = tc::component_geometry::setFootprint(geometry, custom_id, 6.f, 3.f);
}
```

半宽会向外取整到整数格（最小 `0.5`，单边最大 `16383.5`）；旧类型不调用时原型完全不变。
调用只允许在游戏线程且该 Mod 的加载窗口内，其他 Mod 的 id 返回 `ERR_OWNERSHIP`。给
`read_footprint` 一个实时元件 `TCGameHandle` 可读取游戏当前真正使用的矩形；90°/270° 会交换
宽高，过期句柄返回 `ERR_STALE`。SDK 表与错误文字封装在 `sdk/tc_component_geometry.h`。

### 声明 footprint 的边界：不要越过引脚（2026-09-25 复核）

把"元件想画多大"和"footprint 该多大"当成同一件事，是这个仓库里**重复踩过两次**的坑：

- 引脚位置由**类型自己声明的通道**决定，**与 footprint 无关**：默认在 `(2,0)`、`(2,-1)`……
  （各方向一样），声明 `pin_lane = 3.0` 的类型（浮点族，也就是原版 Constant/Static Value 那种尺寸）
  在 `(3,0)`、`(3,-1)`……。2026-09-25 实测：把半宽从 2.0 改成 2.5、再改成 3.5，沙盒报告里的
  `out0=` 只跟通道走，不跟 footprint 走。
- footprint 是游戏**命中、选中、拖动和占位**用的框。它一旦越过引脚那条 x，玩家从引脚**拉线就会被
  判成选中/拖动元件**，而且元件右侧会多出一片被占住、却什么都不画的格子。
- 因此：**有引脚的元件，半宽不要超过自己那条通道**——默认通道是 2.0（互动元件对与时钟元件都取
  2.0，前者是手测确认过的值），声明 `pin_lane = 3.0` 的类型可以用到 3.0：浮点族的本体就是原版尺寸的
  4.92 格宽，所以它把半宽声明成 2.5，右侧留给引脚 0.5 格；**高度不受此限**，可以按本体需要给。
  装饰性元件（无脚）不受限制，文本框就是 8×4。

Loader 现在会在 `set_footprint` 时自动检查并**告警**（不拒绝：无脚元件与特殊形状仍合法），
比较的是**该类型自己的通道**（`TCComponentTypeDefinitionV2::pin_lane`）：

```text
Component geometry: the footprint declared for 0x434c4f4b5f303031 is 7 cells wide (half 3.5),
which reaches past the pin lane at 2.  This component now shares the pins' click and drag area,
so a wire can no longer be started from a pin; ...
```

判据本身也进了单测（`tests/component-geometry.cpp` 的 `footprintReachesPins` 真假表），
元件侧的不变量进了 `tests/clock-period.cpp`（本体必须落在 footprint 内、右侧留出引脚通道、
半宽不超过 2.0）。时钟元件那次事故与结论见
[research/custom-component-pins.md](../research/custom-component-pins.md)。

V2 保留完整 V1 表前缀，并增加运行期调用的 `set_instance_footprint`。它接受实时元件句柄和板面坐标
半宽高，把矩形接到游戏原生 `get_component_id` 点查询；命中后游戏照常完成选择、拖动、删除和属性
面板。这个逐实例矩形不写回 Prototype，因此不改变类型级占位/放置碰撞：适合文本框这类可见尺寸会
变化、但不应把整片覆盖区域都设成不可放置的元件。

```cpp
tc::component_geometry::ApiV2 geometryV2{};
if (tc::component_geometry::tableV2(host, &geometryV2)) {
    // 可在棋盘运行时反复更新，例如每帧同步可见文本框。
    tc::component_geometry::setInstanceFootprint(geometryV2, component, 5.5f, 2.8f);
}
```

`examples/text-box` 保留 8×4 类型 footprint 负责占位，并把每个实例最终渲染出来的宽高实时上报 V2；
整张可见文本框因此都能原生选中和拖动，不再依赖插件自己的点击覆盖层或虚拟选中态。

V3 再保留完整 V2 表前缀，并增加**按整格声明**的 `set_footprint_cells(custom_id, x, y, width,
height)`：x/y 是相对元件自己那格、以格为单位的左上角（+y 向下，与引脚同一套约定），width/height 是
整格数。这条通路是为"本体不居中"的元件加的：**游戏把生成的引脚从第 0 行往下排**，所以一边引脚更多的
元件，本体会落在自己那格的下方——四输出的比较器本体覆盖 -0.33…3.33 格，即第 0…3 格。用居中形式声明
时宿主会把半高**向外取整**，结果要么在本体上方多留一整格、要么漏掉本体下沿；而矩形就是游戏命中、
选中、拖动、占位用的框，两种错法玩家都看得见（`tests/float-pitch-playtest.ps1` 量出来的"元件贴不到
一起"就是这么来的）。单元格形式按声明原样存储，不取整。

```cpp
tc::component_geometry::ApiV3 geometryV3{};
if (tc::component_geometry::tableV3(host, &geometryV3)) {
    // 原版尺寸的本体（4.92 × 2.93 格）占 -2..2 × -1..1，正好五乘三格。
    tc::component_geometry::setFootprintCells(geometryV3, custom_id, -2, -1, 5, 3);
}
```

两种形式都写同一条原型矩形序列，也共用"不要越过引脚通道"的告警；宽度方向的规则不变（半宽不要超过
自己的通道），高度方向按本体需要给——只是现在可以**带着偏移**给。

## tc.component.render：棋盘覆盖绘制

`tc.component.render` V1 是 M5 的最小覆盖层切片。Mod 在 `tc_mod_load` 期间为自己拥有的类型登记
回调；宿主每个棋盘帧枚举该类型的实例，传入帧级 `TCGameHandle`、custom/instance id、旋转、
局部到屏幕的二维仿射基和屏幕裁剪框。局部点按
`origin + x * axis_x + y * axis_y` 映射，轴向已包含相机缩放、DPI 与四向旋转。

```cpp
#include "tc_component_render.h"

static void draw(void*, const TCComponentRenderFrameV1* frame) {
    float x1, y1, x2, y2;
    tc::component_render::localToScreen(*frame, -2.f, -1.f, &x1, &y1);
    tc::component_render::localToScreen(*frame,  2.f,  1.f, &x2, &y2);
    frame->draw->line(frame->draw->context, x1, y1, x2, y2, 0xffffffffu, 2.f);
}
```

V1 提供线、描边/填充矩形、描边/填充圆和默认字体文本。宿主不把私有 ImDrawList/renderer 指针
交给 Mod；调用只在回调内有效，每实例每帧最多 4096 条命令，非法浮点、负尺寸和过长文本会被拒绝。
`clip_min/max` 不只是提示：宿主在进入回调前把同一矩形压入绘制列表，返回后恢复；跨越四边的图元
只保留框内像素。离开棋盘后没有实例枚举，因此回调立即停止。当前 V1 **只做覆盖绘制**：关闭游戏默认缩略图/水印、
纹理/字体资源生命周期和设备重建通知仍是后续切片。

### 绘制表 V2：按像素字号画字（板面文字要和原版一样大）

绘制表本身也是**追加入口**的：`frame->draw` 指向的表永远是这一版宿主拥有的最新一张，Mod 先读
`draw->version` / `draw->size`，再决定要不要用它（`sdk/tc_component_render.h` 的 `drawV2()` 封装
了这一步）。V2 追加两个入口：

```cpp
using tc::component_render::textSized;
using tc::component_render::measureText;

/* 一格 = |axis_x| 像素；原版 Constant 的值 0.59 格高、名字 0.40 格高。 */
const float cell = std::sqrt(frame->axis_x_x * frame->axis_x_x +
                             frame->axis_x_y * frame->axis_x_y);
float width = 0.f, height = 0.f;
if (measureText(*frame, 0.40f * cell / 0.50f, true, "CONST", &width, &height)) {
    /* width/height 就是 textSized 用同一个 size 画出来会占的像素 */
    textSized(*frame, x - width, y, 0.40f * cell / 0.50f, 0xffffffffu, true, "CONST");
}
```

- `text_sized` 用**显式像素字号**画一行字：本构建的 `ImDrawList::AddText_Vec2` 没有字号参数，
  宿主把绘制列表共享数据里的字体/字号**换一次、画完立刻换回**（`text-box` 例子早就这么干，
  这里是同一个做法进了服务）。`bold` 选游戏自己的粗体面（`NoroshiCode_Bold`，元件标签就是它），
  否则沿用当前面。
- `measure_text` 回答"这个字符串用这个字号画出来会有多大"，与 `text_sized` 用同一套字体与缩放，
  所以右对齐名字、把过长的值缩到本体宽度以内都不用 Mod 自己估字宽。
- 两个入口都不改宿主之外的状态：字号/字体只在这一次调用内生效，同帧其它文字不受影响；
  老 loader 的 `frame->draw` 里没有这两项，Mod 可以退回普通 `text`（`drawV2()` 返回 null）。

**实测换算**（`local.float-ops` 的标定，见 [research/custom-component-pins.md](../research/custom-component-pins.md)
与 [../verification.md](../verification.md)）：本构建按请求字号的 0.79 倍绘制，画出来的数字高
约是这一版的 0.66 倍——两者相乘 0.52，也就是"数字高 0.59 格"对应请求字号
`0.59 × 每格像素 ÷ 0.52`。宿主在 CPU 侧不变，这两个数是**当前构建**的测量值，换构建要重新量。

### V2：关掉游戏自己的元件图案

V2 保留完整的 V1 前缀，只多一个入口，用来取代游戏给自定义元件画的设计图缩略图、名字水印和
相关默认网格：

```cpp
tc::component_render::ApiV2 render{};
if (tc::component_render::tableV2(host, &render)) {
    tc::component_render::setDrawCallback(render, custom_id, &draw);   /* V1 的覆盖回调 */
    tc::component_render::setDefaultDrawing(render, custom_id, false); /* V2：关掉游戏图案 */
}
```

语义（真机证据见 [../verification.md](../verification.md) 的 "M5 render V2" 一节）：

- 按**类型**生效，只能关调用 Mod 自己登记的类型；再次传入 `1` 恢复。
- 只对带实例 id 的**棋盘实例**生效。元件栏/元件选择器用 id 为 0 的临时记录预览，仍然照常画图案，
  所以关掉以后元件在菜单里不会变成空白。
- 只影响"游戏画什么"。footprint、命中与拖动、模型里的引脚、选中/删除、以及本 Mod 自己的 render
  回调都不受影响；关掉图案后点击、拖动、选中和取消选中都已在真机断言。
- 游戏自己画的**选中提示 UI**（元件外侧的白色断环）由 V3 接管（见下）；引脚标签仍然属于游戏自己
  的交互提示层，不在 V2 范围内。

### V3：关掉游戏自己的选中提示

V3 保留完整的 V2 前缀，只多一个入口。游戏给选中元件画的那圈白色断弧是**固定缩放**的一个精灵实例
（尺寸跟 level tree 的上下文走，既不跟元件网格也不跟 footprint），所以"让它跟着 footprint 变"做不到；
能做的并且已经做到的是**按类型把它关掉，由 Mod 自己画**：

```cpp
tc::component_render::ApiV3 render{};
if (tc::component_render::tableV3(host, &render)) {
    tc::component_render::setDrawCallback(render, custom_id, &draw);
    tc::component_render::setDefaultDrawing(render, custom_id, false);
    tc::component_render::setSelectionHint(render, custom_id, false); /* V3 */
}
```

语义（真机证据见 [../verification.md](../verification.md) 的「游戏自己那圈白弧已按类型关掉」）：

- 按**类型**生效，只能关调用 Mod 自己登记的类型；再次传入 `1` 恢复。
- 只在"该类型真的被选中、且游戏正在画选中精灵"的绘制帧里生效：宿主把该元件在游戏选中集里的那一条
  占位字临时清 0，游戏自己的绘制循环因此不为它产生精灵实例，调用返回后立刻还原。别的元件、导线、
  内置元件以及选中状态本身都不受影响。
- 与 V2 一样只改"游戏画什么"：选择、命中、拖动、删除、默认绘制与 footprint 都不变。Mod 想保留游戏
  那圈弧线作对照时不调用即可。

已交付但**当前没有任何示例在棋盘上使用**的接口（例如原版那圈提示的模仿、V1 的几个未用图元、
`set_selection_hint(…,1)` 的恢复路径）登记在
[../reference/unused-interfaces.md](../reference/unused-interfaces.md)：那里写了参数、证据和接回来的步骤。

### V4：按类型去掉"在元件工坊编辑"按钮

V4 保留完整的 V3 前缀，只多一个入口。选中一个**自定义**元件时，游戏会在底部元件面板的右上角画一个
按钮，点它就把这个元件的原理图载入元件工坊：

```cpp
tc::component_render::ApiV4 render{};
if (tc::component_render::tableV4(host, &render)) {
    tc::component_render::setDrawCallback(render, custom_id, &draw);
    tc::component_render::setDefaultDrawing(render, custom_id, false);
    tc::component_render::setSelectionHint(render, custom_id, false);
    tc::component_render::setFoundryButton(render, custom_id, false); /* V4 */
}
```

语义（真机证据见 [../verification.md](../verification.md) 的「按类型去掉"在元件工坊编辑"按钮」）：

- 按**类型**生效，只能关调用 Mod 自己登记的类型；再次传入 `1` 恢复。
- 只在"当前选中的元件全是这些类型"时隐藏：混选里有一个游戏自己的元件，按钮就照常出现，免得玩家
  失去进去编辑它的入口。
- 隐藏的做法是在面板那一次按钮调用上用全透明样式色绘制并返回"未点击"：**布局与其它控件完全不变**
  （按钮仍然占位），只是看不见、点不动；实测同一矩形默认 0 像素、对照 654 像素。
- 只影响这个按钮。选择、命中、拖动、删除、默认绘制、选中提示、footprint，以及工坊本身（含工具栏里
  打开工坊的按钮）都不变。

### V5：元件栏卡片与抽屉预览画 Mod 自己的图

游戏给一个自定义元件画的那张小图（元件栏里的一项、底部抽屉里的预览）来自**一张它自己每帧请求的
纹理**：`get_captured_path__presenterZio_u28` 按 kind 分派，自定义原型（`0x4e`）得到
`?snapshot_cc/com_custom_<十进制 id>.png`，再经 `get_asset_path` 落到
`<游戏目录>/asset/?snapshot_cc/com_custom_<id>.png`。往这条路径上放文件**没有用**（游戏会读、
会重编码，但画出来的仍是它自己渲染的设计图缩略图 + 名字水印），所以 V5 给 Mod 的是"这张图由你提供"：

```cpp
tc::component_render::ApiV5 render{};
if (tc::component_render::tableV5(host, &render)) {
    tc::component_render::setDrawCallback(render, custom_id, &draw);
    tc::component_render::setDefaultDrawing(render, custom_id, false);
    tc::component_render::setSelectionHint(render, custom_id, false);
    tc::component_render::setFoundryButton(render, custom_id, false);
    tc::component_render::setPicture(render, custom_id, "C:\\mods\\fp32-add.png"); /* V5 */
}
```

语义（真机证据见 [../verification.md](../verification.md) 的「元件图片 V5」一节，测量过程见
[research/component-icons.md](../research/component-icons.md)）：

- `png_path` 是**绝对**路径；传 `NULL`（或游戏真要这张图时文件已经不在）就把这个类型放回游戏自己的
  渲染。加载期调用一次即可，之后**改写这个文件**也会被下一次请求读到（加载器按大小/时间戳判断）。
- 加载器**不把 Mod 的文件直接交给游戏**：它先复制到 `<游戏目录>/tc-modloader-data/pictures/<id>.png`
  再把副本路径交给游戏——游戏读完会把重编码结果写回它读的那个路径，而 Mod 的包内文件由加载器的
  文件台账（哈希）管着，被外部改写会让下一次 apply 拒绝。失败一律回退，不抛异常。
- 覆盖的面：**元件栏卡片 + 底部抽屉预览**（两处都是同一张纹理）。放置幽灵不使用这张纹理；需要
  `TCComponentRenderApiV6::set_placement_preview` 接管（见下节）。
- 只对调用 Mod 自己登记的类型生效；`shape_svg`、`set_default_drawing` 都改不动这张图（前者对自定义
  原型无效，后者只管棋盘上的默认绘制）。没有 V5 的加载器不发布这张表，`tableV5()` 返回 false。

### V6：用同一个绘制回调接管放置幽灵

V6 保留完整 V5 前缀，新增按类型选择的 `set_placement_preview`。启用后，加载器在游戏的
`redraw_clipboard_component` 路径拿到临时自定义元件的吸附坐标和旋转，抑制游戏默认的“仅引脚/名字”
幽灵，并在每帧调用该类型已经注册的 `set_draw_callback`：

```cpp
tc::component_render::ApiV6 render{};
if (tc::component_render::tableV6(host, &render)) {
    tc::component_render::setDrawCallback(render, custom_id, &draw);
    tc::component_render::setPlacementPreview(render, custom_id, true);
}
```

预览帧用 `instance_id == 0` 表示“没有已落盘实例”，`component` 是零句柄，`config/config_size` 为空；
回调应使用类型默认配置，并且不要把这个哨兵 id 写进实例缓存。坐标、旋转、轴和裁剪与棋盘实例相同。
加载器持续提交即时绘制，直到游戏**把剪贴板记录放下**为止——它每帧读游戏自己的
`update_state_clipboard` 交出来的那份记录（kind `0x4e` + 本类型 custom id = 还在放置；空记录或换了
元件 = 落下/取消/换了选择），`hide_clipboard` 钩子只作兜底。所以 `instance_id == 0` 的预览帧只在
“这次放置还活着”时出现，成功放置后不会再画。传 `false` 恢复游戏自己的幽灵；能力只允许 Mod 自己
登记的类型。真机 `picture-playtest` 连续四帧都检测到 Float Ops 的紫色 `ABS |x|` 本体和红色引脚、
V5 的品红标记图在幽灵区域为 0（两个接管面彼此独立），并在取消与真正落下之后各断言一次落点没有残留。

边界（都是实测数字，见 [research/custom-component-pins.md](../research/custom-component-pins.md)）：

| 项 | 值 |
|---|---|
| 每方向脚数 | 0–16（加载器数组上限；游戏导入器到 32 脚都收） |
| 每脚位宽 | 1–64 位（与游戏原版一致） |
| 总输入位宽 | ≤128 位（生成的调用只有两个 64 位 payload 字；六个参数的外调会把第 5、6 个参数丢成垃圾，实测） |
| 空方向 | 一个方向为 0（纯源/纯汇）可以；**两个都空也可以**，用于装饰元件（文本框）：加载器为它补一个不接线的驱动节点，元件因此仍进平坦序列、仍有实例句柄，但既不读也不写任何信号 |
| 状态 | `state_words` 每实例持久字，0 表示无状态；只有 CYCLE 提交 |
| 配置 | `config_size` 0–65536 字节；每实例复制默认值，schema 由定义声明；RESET 不清配置 |
| 多字（>64 位）信号 | **不支持**（按取舍关闭，出口是 M2 的宿主状态/缓冲） |

共享的仿真契约与 V1 完全一致：回调跑在仿真线程、`REFRESH` 跑在副本上不提交、回调抛异常输出归零、
每个输出脚对应定义里的一个可识别驱动门。真机证据：`tests/pin-shape-playtest.ps1 -Shape wide9`
（game 层用例 `pin-shape-wide9`）——九脚定义注册成功、实例绑定、回调每拍收到 9 个输入、
状态按定义的两个字自增。

**生命周期（同一张表里的可选指针）**：定义末尾可以挂一个 `TCComponentLifecycleV1`
（`sdk/tc_service_api.h`），包含两个回调：

```c
static const TCComponentLifecycleV1 lifecycle = {
    sizeof(TCComponentLifecycleV1), TC_COMPONENT_LIFECYCLE_VERSION_1,
    &onCreate, &onDestroy, &onConfigChanged, &onClone, &onLoad, &onSave
};
definition.lifecycle = &lifecycle;    /* definition.size 已覆盖这个字段时才会被读取 */
```

- `on_create`：实例**首次绑定**时触发一次（编译发现它时），相位 `TC_LOGIC_CREATE`；
- `on_destroy`：实例被删除、或离开棋盘/切场景时触发，相位 `TC_LOGIC_DESTROY`；
- `on_config_changed`（2026-09-22 追加在结构末尾）：实例的配置**被替换**时触发，相位
  `TC_LOGIC_CONFIG_CHANGED`，回调里 `config/config_size/config_schema` 是**新**字节。触发点：
  一次成功的 `write_config`；以及一次撤销/重做把配置改回旧值。**不**触发的情形：写入与当前相同
  的字节；装载存档里的记录或迁移旧 schema（那是绑定的过程，发生在 `on_create` 之前，回调在
  `on_create` 里就能看到那些字节）。
- `on_config_changed` **不持有实例锁**：回调里可以调用 `tc.component.storage`（`info`/`read_config`
  等）。`on_create`/`on_destroy` 目前仍在实例锁内运行，别在它们里面调服务。
- `on_clone`（2026-09-22 追加在结构末尾）：实例是宿主**复制**出来的，相位 `TC_LOGIC_CLONE`。
  它在 `on_create` **之后**、且副本配置已经就位时触发，所以回调里 `config` 就是源实例那份字节
  （不是注册时的默认值）；插件可以在这里发一个新身份（那次改动是普通的 `write_config`，自己占
  一条撤销）。复制本身走命令总线：

```cpp
TCCommandV2 duplicate{};
duplicate.size = sizeof(duplicate);
duplicate.type = TC_COMMAND_BOARD_DUPLICATE_COMPONENT;
duplicate.subject = board;                    // Board 句柄
duplicate.custom_prototype_id = myTypeId;     // 源元件的类型
duplicate.argument = sourceInstance;          // 源元件的实例 id
duplicate.x = x; duplicate.y = y; duplicate.rotation = rotation;   // 目的地
commandApi.submit(commandApi.context, &duplicate, &request);
```

  放置由游戏自己的助手完成，所以**一次 Ctrl+Z 就撤掉整次复制**（副本连配置一起消失）；配置副本
  是直接写进新记录的，不占自己的撤销步——复制对玩家是一个动作。
- `on_load`（2026-09-22 追加）：实例的配置**来自电路文件里的记录**（原样恢复或经迁移转换），
  相位 `TC_LOGIC_LOAD`。它在 `on_create` 之后（若是复制出来的实例则改用 `on_clone`），回调里
  `config` 就是文件里的字节。**被拒绝的记录不发**——那种情况实例跑的是默认配置。
  注意实现细节：绑定发生在编译的扁平序列上时，棋盘记录可能要到第一次服务调用才读得到，因此
  这个通知也可能在那时才发；两种情况下语义相同。
- `on_save`（2026-09-22 追加）：游戏**即将把电路写出去**，相位 `TC_LOGIC_SAVE`。两个入口都会派发：
  关卡保存（`save.level`，事件总线本来就监听它）与原理图写入（`save.schematic`）。回调里可以
  直接把派生数据或修复后的配置用普通 `write_config` 提交，**正在写的文件就会看到它**。
  `on_load`/`on_clone`/`on_create`/`on_destroy` 仍在实例锁内运行（别在里面调服务）；`on_config_changed`
  与 `on_save` 在锁外运行（可以调服务）。
- 两者都在"变化发生的地方"同步调用（编译线程或场景切换），因此**必须便宜、不得触碰 UI**；
  写入的 `state`/`outputs` 会立即提交（它们不跟随某个周期）；
- 老的调用方（`size` 停在 `lifecycle` 之前）不会收到任何通知，行为与之前完全一致。
- `TCLogicIOV2` 尾部的 `config/config_size/config_schema` 是宿主借用的只读视图；新回调先用
  `io->size` 判断尾字段是否存在，老回调不受影响。

## tc.component.instances：跨帧稳定的实例句柄

`instance_id` 单独用不住——游戏会复用槽位，前几帧记下的 id 可能指向另一个元件。句柄因此带上
**宿主发放的 generation**：不匹配就返回 `ERR_STALE`，而不是解析成"现在恰好在那里的东西"。
SDK 封装在 `sdk/tc_component_instances.h`：

```cpp
#include "tc_component_instances.h"

tc::component_instances::Api instances{};
if (tc::component_instances::table(host, &instances)) {
    TCComponentInstanceHandle handles[8];
    uint32_t written = 0, total = 0;
    tc::component_instances::enumerate(instances, kMyCustomId, handles, 8, &written, &total);
    for (uint32_t i = 0; i < written; ++i) {
        TCComponentInstanceInfoV1 info{}; info.size = sizeof(info);
        tc::component_instances::info(instances, handles[i], &info);      // 脚数/状态字数/调用计数
        uint64_t state[8]; uint32_t words = 0;
        tc::component_instances::state(instances, handles[i], state, 8, &words);
    }
    tc::component_instances::reset(instances, handles[0]);                // 只重置这一个实例
}
```

| 入口 | 语义 |
|---|---|
| `enumerate(custom_id, out, capacity, written, total)` | 活实例的句柄；`custom_id` 为 0 表示全部；缓冲不够返回 `ERR_RANGE` 并在 `total` 里给出总数 |
| `validate(handle)` | 句柄还指向那个实例就 `OK`，槽位被重新绑定过就 `ERR_STALE` |
| `info(handle, out)` | 脚数、`state_words`、`cycle_calls`/`peeks`/`resets` 计数、类型 id 与所属 Mod |
| `state(handle, out, capacity, words)` | 宿主状态 blob（最旧的字在前）；缓冲不够返回 `ERR_RANGE` 并给出真实字数 |
| `reset(handle)` | 只对这一个实例跑一次 RESET 回调并清零其状态；**不**动仿真 |

失效规则（写清楚，因为它是"稳定引用"的全部意义）：

1. 编译结果里出现**本类型至少一个实例**时，这个编译就是棋盘本身——我们的实例里没出现的那几个
   被释放（触发 `on_destroy`），`bindingKeys` 里对应的键删掉；
2. 离开棋盘/切场景时全部释放；
3. 释放的实例**保留槽位**（标记为不活跃），所以其它实例的 token 不会移动；槽位被下一个绑定
   复用时发放新的 generation——这正是旧句柄失效的机制；
4. 关卡自己的测试台程序也会编译，但它不含我们的实例：第 1 条只在这种"确实是棋盘"的编译上生效，
   否则每加载一关都会把活实例拆掉。

## tc.component.storage：配置与仿真状态快照

`tc.component.storage` V1 把此前统称为“持久状态”的内容拆成两类：

- **配置**：类型定义提供默认 blob 和 schema；每个实例持有独立副本，RESET 后仍保留；
- **仿真状态**：就是回调的 `state_words`，RESET 会清零并运行 RESET 回调。

SDK 封装在 `sdk/tc_component_storage.h`：

```cpp
#include "tc_component_storage.h"

tc::component_storage::Api storage{};
if (tc::component_storage::table(host, &storage)) {
    TCComponentStorageInfoV1 info{}; info.size = sizeof(info);
    tc::component_storage::info(storage, handle, &info);

    std::vector<uint8_t> config(info.config_size);
    tc::component_storage::readConfig(storage, handle, config.data(), config.size());
    config[0] = 2;
    tc::component_storage::writeConfig(storage, handle, info.config_schema,
                                       config.data(), config.size());

    std::vector<uint8_t> checkpoint(info.state_size);
    tc::component_storage::captureState(storage, handle,
                                        checkpoint.data(), checkpoint.size());
    tc::component_storage::restoreState(storage, handle,
                                        checkpoint.data(), checkpoint.size());
}
```

| 入口 | 语义 |
|---|---|
| `info` | schema、配置/状态字节数、配置 revision 与原实例句柄 |
| `read_config` | 读配置；`out=nullptr, capacity=0` 可先查大小 |
| `write_config` | 整 blob 原子替换；schema 或长度不匹配会拒绝，回调下一次运行时看到新值 |
| `capture_state` | 复制当前仿真状态；同样支持先查大小 |
| `restore_state` | 原样恢复同尺寸状态，不运行 RESET，也不改配置 |

所有入口都重新校验实例 generation；旧句柄返回 `ERR_STALE`。每个实例配置上限 64 KiB（内存层）。

**一次有效的 `write_config` 之后再让棋盘重算一次，是写配置那个 Mod 自己的事。** 游戏的编译器不知道
自定义元件的配置变了（它只认识自己的原件），所以改完配置以后，暂停中的板子会一直显示上一次算出来的
值，直到玩家按刷新——玩家把它描述成"改完要刷新一下才更新"。宿主**不会**替 Mod 刷新：加载器侧试过
两种自动判断（看最后一条 `sim.do` 命令、每帧读 `sim.cycle`）以及无条件刷新，三种都扰动过别的用例
（第一种判断错，后两种会打断 `text-box` 的相机平移，见 [../verification.md](../verification.md)）。
正确的做法是写完配置后由 Mod 发出玩家自己那个请求：`tc::simulation::pause(sim)`（= `sim.do`
command 1，走同一条钩子链），`local.float-ops` 就是这么做的
（`examples/float-ops/components.cpp` 的 `refreshBoardAfterEdit`，日志
`float-ops: the board was refreshed after a configuration edit (status 0)`）。

### 配置随元件存档（2026-09-22 第二刀）

配置**不再只活在内存里**：它写进元件记录自己拥有的那张 64 位键值表，也就是游戏
`save_monger/versions/v7` 的 `Table[int64, int64]`。游戏把它作为元件记录的一部分序列化进原理图，
所以配置随复制、存档和关卡加载一起走，不需要外置 sidecar。

| 项目 | 行为 |
|---|---|
| 绑定时机 | 实例被绑定时读回记录；记录完整且校验通过才覆盖 `default_config`，因此 `on_create` 就能看到存档里的配置 |
| 写回时机 | `write_config` 先提交记录、再改内存；记录写不进去时整次调用失败，内存里的配置保持旧值 |
| 提交顺序 | 先写数据分块，最后写 schema/长度与校验和。半途失败留下的是**校验不通过**的记录，不是"看起来有效但只写了一半"的配置 |
| 记录的键空间 | 高 32 位是 `TCM3` 魔数、低 32 位是字段号（格式、定义 id、schema+长度、校验和、数据分块从 `0x1000` 起）。不认识的表项原样保留 |
| 拒绝而非覆盖 | 记录来自别的定义、格式未知或校验和不符时，读回被拒绝、报告原因、旧字节保留；schema/长度不符会先交给迁移回调（见下一节），迁移没成功也同样保留原字节 |
| 可用性 | `info.flags` 增加 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE`。构建画像没有描述记录布局、或该调用不在游戏线程上时，这一位为空，配置退回内存层 |
| 上限 | 可存档配置 **1024 字节**（远比内存层的 64 KiB 小），见 [limits.md](../reference/limits.md) |

```cpp
TCComponentStorageInfoV1 info{}; info.size = sizeof(info);
tc::component_storage::info(storage, handle, &info);
if (info.flags & TC_COMPONENT_STORAGE_HAS_PERSISTENCE) {
    // 这次写入会随原理图存档，并在此元件下一次载入时回到实例上
    tc::component_storage::writeConfig(storage, handle, info.config_schema,
                                       config.data(), config.size());
}
```

### schema 迁移（2026-09-22 第三刀）

存档里的记录带着它写入时的 schema 编号，所以 Mod 改了配置布局以后，老电路里就是"读不懂的字节"。
定义可以声明一个迁移回调，宿主在**实例绑定时**把它交给定义去转换：

```cpp
static int migrate(void* /*user*/, uint32_t from_schema, const void* from_data, uint32_t from_bytes,
                   uint32_t to_schema, void* out, uint32_t capacity) {
    if (from_schema != 6 || from_bytes != 4) return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    const uint8_t* old = static_cast<const uint8_t*>(from_data);
    uint8_t* next = static_cast<uint8_t*>(out);
    next[0] = old[0]; next[1] = old[1]; next[2] = 0; next[3] = old[3];   // 新布局
    return TC_COMPONENT_CONFIG_MIGRATE_OK;
}

definition.config_migration_version = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
definition.migrate_config = &migrate;
definition.migration_user = myContext;   // 第一个参数原样回传
```

| 结果 | 含义 |
|---|---|
| `MIGRATE_OK` | `out` 里的 `capacity` 字节成为实例的配置；宿主给它重算校验和、把记录升级成新 schema，**下一次存档**就带走新格式 |
| `MIGRATE_KEEP` | 不改动：记录保留原字节，实例跑注册的默认配置 |
| `MIGRATE_REJECT` | 同上，但日志写明"拒绝升级"——表示这次升级作者不打算支持 |
| 其它返回值 | 一律按 `REJECT` 处理，坏回调不会被误当成升级成功 |

规则：

- 回调拿到的是**电路里真正存着的字节**（已通过校验和验证），不是默认值；`from_schema`/`from_bytes`
  就是它写入时的样子，可能和当前 `config_size` 不同。
- 只有"同一个定义 id + schema 或长度不符"的记录会送到回调。来自别的定义、校验和不符、格式号
  未知的记录不会——那不是迁移问题。
- `KEEP`/`REJECT`（以及没有声明迁移的定义）都**不写回**：旧字节原样留在电路里，所以作者以后
  补一个正确的迁移仍然来得及。
- 回调在游戏线程、实例绑定时调用，可能发生在关卡载入的编译过程中。

### 配置写入与撤销/重做（2026-09-22 第五刀）

**一次成功的 `write_config` 就是一次可撤销的配置提交。**

游戏自己的撤销栈没有"配置变更"这种条目：它的变更种类是棋盘编辑（放置/旋转/删除/改尺寸/导线…），
而写值的命令一条都不登记。所以宿主自己保存这一步的**前后字节**，并在游戏撤销入口被按下时，只要
还有未消费的配置步骤，就把它写回元件记录并报告"已处理"：

```text
write_config(A)  -> 记录 = A，压入一步 {before, after}
write_config(B)  -> 记录 = B，压入一步 {before=A, after=B}
Ctrl+Z           -> 记录回到 A（宿主回答这次按下，不动游戏自己的栈）
Ctrl+Y           -> 记录回到 B
```

| 项目 | 规则 |
|---|---|
| 粒度 | 一次成功写入 = 一步；写入相同字节不产生步骤 |
| 顺序 | 步骤是 LIFO；新写入清空重做栈 |
| 与棋盘编辑的关系 | 只有存在未消费的配置步骤时宿主才接管这次 undo/redo；栈空时**原样交给游戏**，棋盘编辑的手感不变 |
| 生命周期 | 最多保留 64 步；实例被释放或换板时丢弃相关步骤（记录已不存在，旧 id 可能指向别的元件） |
| 读取判据 | 断言这类行为要看**元件记录里的表**，不能看 `tc.component.storage`：绑定会保留内存副本，服务读到的可能不是棋盘上的字节 |

仍然**没有**：把配置提交与棋盘编辑合并成同一组（`tc.transactions` 目前只暂存命令）、缺失 Mod 期间
画面上的占位元件（已并入 M5）、复制（`on_clone`）。

### 配置编辑事务（`tc.component.storage` V2）

计划 §9.3 要求批量工具**显式说明动作从哪里开始、到哪里结束**，而不是让宿主按帧边界猜分组。
V2 在 V1 的五个入口后追加三个：

```cpp
tc::component_storage::ApiV2 storage2{};
if (tc::component_storage::tableV2(host, &storage2)) {
    tc::component_storage::beginEdit(storage2, handle);      // 记住此刻的配置
    tc::component_storage::writeConfig(...);                 // 中间任意多次写入
    tc::component_storage::commitEdit(storage2, handle);     // 整段算作一条撤销
    // 或者
    tc::component_storage::abortEdit(storage2, handle);      // 回到 begin 时的字节，不产生撤销
}
```

| 项目 | 规则 |
|---|---|
| `begin_edit` | 记住实例当前的配置字节；同一句柄上已有事务时返回 `ERR_STATE`（不嵌套） |
| 事务中的写入 | 照常改配置、照常发 `on_config_changed`，但**不**各自压栈 |
| `commit_edit` | 把整段压成**一步**（begin 时的字节 → 当前字节）；没有事务时返回 `ERR_STATE` |
| `abort_edit` | 把配置还原成 begin 时的字节（记录与宿主内存都改），撤销栈不动；没有事务时返回 `ERR_STATE` |
| 生命周期 | 实例被释放或换板时丢弃未提交的事务 |
| 上限 | 与单次写入共用同一个栈：每块板最多 64 步 |

## 演进规则

- 已发布服务表的字段、顺序、类型和常量不能修改。
- 扩展可以定义新版本表；V1/V2 必须继续可查询。
- 服务版本不等于 Loader 版本，也不要求其他服务同步升级。
- 不在表中暴露 STL 类型、异常、借用字符串或容器迭代器。
- 输出数据若需跨调用存活，必须复制进调用方缓冲区或返回带代次的句柄。
