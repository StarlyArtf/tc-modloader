# 研究交接：0 脚元件的导入与调度

更新时间：2026-09-22

计划出处：[PLAN-custom-components.md](../PLAN-custom-components.md) 阶段 1；上一份交接把它列为
"必须先有答案或明确标记不可用"的三个待解问题之首
（[HANDOFF-component-registry.md](../HANDOFF-component-registry.md)）：**N=0（纯源）时谁触发
驱动门？M=0（纯汇）时谁来调用回调？** 本文只记录这一轮的测量与由它得出的设计结论；
`>8 脚` 与 `>64 位` 仍是待解问题。

## 要回答的问题

生成脚手架的形状是"n 个输入收集门 + n-1 个依赖门 + m 个驱动门"
（`src/component_definition.hpp`）：输入收集门把调用顺序串起来，驱动门才调用回调。于是：

- **源（0 输入，m 输出）**：没有收集门、没有依赖链，驱动门的输入**悬空**。编译器会不会直接
  把这个门剪掉？剪掉就永远没有回调。
- **汇（n 输入，0 输出）**：没有输出脚，也就没有"驱动门"。给一个输出悬空的假驱动门，
  编译器会不会保留它？保留不下来，汇元件就无从运行。

这两件事都只能由游戏回答，所以先测量，再决定 V2 的引脚／值模型。

## 这一轮做了什么

| 位置 | 内容 |
|---|---|
| `src/component_definition.hpp` | `valid()` 允许**一个**方向为空，也允许**两个都空**（装饰元件，2026-09-23 复测确认：0 进 0 出仍会进平坦序列并绑上实例）；`encode()` 为 0 输入不再生成收集门与依赖链，为 0 输出补一个输出悬空的驱动门 |
| `src/native_logic.hpp` | `supportedScaffold()` 接受 0 输入或 0 输出；实例节点数期望改为 `prefix + outputs + (outputs==0 ? 1 : 0)`，其中 `prefix = inputs ? 2*inputs-1 : 0`；汇元件那一个尾节点按"第一个驱动门"发回调 |
| `tests/native-component.cpp` | 离线断言两种脚手架的节点序列：源 = `[0x12, 0x51]`，汇 = `[0x4f, 0x12, 0x12]`，三输入汇 = 9 个节点 |
| `tests/pin-shape-probe.cpp` | 真机探针：复用 `example.byte-adder` 的关卡运行器，只替换注册，逐个形状注册并记录每次回调 |
| `tests/and-component-fixture.cpp` | 两块板子 `build/nl_src0board.data`、`build/nl_sink0board.data` |
| `tests/pin-shape-playtest.ps1` | 真机用例（`-Shape source|sink`）：把板子放进关卡、跑起来、把证据打印出来 |

## 真机证据

探针一次运行同时注册两种形状（都在 `tc_mod_load` 期间；`register_component` 的返回值就是
游戏的答案）：

```text
Native logic: registered custom 0x535243305f303031 inputs=0 outputs=1 out0=(2,0,w1) shape=0in/1out
pin-shape: source registration (0in/1out) -> 0
Native logic: registered custom 0x534e4b305f303031 inputs=1 outputs=0 in0=(-2,0,w1) shape=1in/0out
pin-shape: sink registration (1in/0out) -> 0
```

**导入**：两种形状都被游戏接受（返回 0，原型可读回，引脚几何正确）。

### 源：输入悬空的驱动门被保留，并且每拍执行一次

`nl_src0board.data`（`or_gate` 关卡：电平输入 → 内置 OR → 元件输出 → 关卡输出）编译后的棋盘：

```text
Native logic: board layout node0=0x0/parent=0x0 node1=0x4e/parent=0x2222222222222222
  node2=0x6/parent=0x0 node3=0x3f/parent=0x0 node4=0x44/parent=0x0
  node5=0x12/parent=0x2222222222222222
Native logic: bound instance 0x2222222222222222 of custom 0x535243305f303031 as token 1
pin-shape: source cycle=0 ... calls=1
pin-shape: source cycle=1 ... calls=2
pin-shape: source cycle=2 ... calls=3
pin-shape: source cycle=3 ... calls=4
```

`node5=0x12` 就是那个**输入悬空的驱动门**：它没有被剪掉，桥接按 `0 + 1 = 1` 个逻辑节点绑定了
该实例，回调**每拍恰好一次**（cycle 与 calls 同步递增）。

### 汇：输出悬空的假驱动门同样被保留

`nl_sink0board.data`（`not_gate` 关卡：电平输入 → 内置 NOT → 关卡输出；同一输入的另一端口接
元件输入）：

```text
Native logic: board layout node0=0x0/parent=0x0 node1=0x4e/parent=0x2222222222222222
  node2=0x3c/parent=0x0 node3=0x44/parent=0x0
  node4=0x50/parent=0x2222222222222222 node5=0x12/parent=0x2222222222222222
  node6=0x12/parent=0x2222222222222222
Native logic: bound instance 0x2222222222222222 of custom 0x534e4b305f303031 as token 1
pin-shape: sink cycle=0 instance=... in=0 calls=1
pin-shape: sink cycle=1 instance=... in=0 calls=2
```

`node5` 是输入收集门，`node6` 是**输出悬空的驱动门**；实例节点数 `2 = prefix(1) + 0 + 1` 与桥接
的期望一致，因此绑定成功、回调每拍一次，输入值也正确（`in=0`，与那时的电平输入一致）。

## 结论（设计决定）

1. **0 脚不需要另开一套调度模型**：给源补一个"悬空输入的驱动门"、给汇补一个"悬空输出的驱动门"，
   游戏自己的编译器会保留并调度它们。V2 的引脚模型可以继续用"收集门 + 依赖链 + 驱动门"，
   只把 n 或 m 允许为 0。
2. **`prefix` 的公式必须是 `inputs ? 2*inputs-1 : 0`**：`2*0-1` 在 `size_t` 下溢，旧公式会把所有
   源元件判成"内部电路不认识"。
3. **值表示不需要提前改成"宿主缓冲 + 显式提交"**：这两个形状都在现有 `tc_logic_invoke` 外调形状
   内解决；把它推迟到真正需要 `>64 位` 或 `>8 脚` 的时候再谈。
4. **关卡自带测试是唯一的坑**：0 脚元件几乎不可能满足战役关卡的期望值，测试一失败游戏就停表
   （实测 `verdict=2`、周期停在 0/1）。所以真机用例里由探针**接管运行**（`simulation.run` 到指定
   周期），而不是等关卡测试跑完——这条对以后所有"元件行为"用例都适用。

## 还没做 / 下一块

| 问题 | 现状 | 下一步 |
|---|---|---|
| `>8 脚` | **游戏侧已测通**（见下）：9 / 16 / 32 输入脚的定义导入全部返回 `status 0`，ID 依次分配。限制完全在我们这边：`kMaxBridgeInputs=8`（数组长度）＋两个 64 位 payload 字（**总输入位宽 ≤128** 才是硬约束） | 把 `kMaxBridgeInputs` 提到 16/32，再用一块"每个脚都接线"的板子做真机运行验证（依赖链长度与操作数顺序） |
| `>64 位信号` | 输入仍走两个 64 位 payload 字，输出仍走 `0x9a0000` 状态槽按位回读 | 先测"3 个 64 位输入"（=192 位）能否打包，再决定是否改宿主缓冲 |
| V2 定义结构体 | 未开始 | 上面两条有答案后再冻结 `TCComponentTypeDefinitionV2` |

### 外调能带几个参数：**4 个**（2026-09-22）

这一条决定了"一个元件最多能带多少位输入"，因此也决定了 >64 位信号那条路要不要走。
测法：`TC_MODLOADER_BRIDGE_WIDE=1` 时，桥接把每次回调的外调从 4 个参数扩成 6 个——
在后面追加两个**与 payload 不同**、由周期推出来的字（`(cycle+1) * 0x9E3779B1` 与
`(cycle+1) * 0x85EBCA6B + 0x165667B1`），入口点再用同一个公式重算并比对。生成源码确认写对了：

```text
var tc_io1 = U1 game_engine.'tc_logic_invoke6'(U64 1, U64 (cycle + 1), U64 (vid270) | ... ,
             U64 0, U64 ((cycle + 1) * 0x9E3779B1), U64 ((cycle + 1) * 0x85EBCA6B + 0x165667B1))
```

但接收端拿到的是垃圾，而且每次都不一样：

```text
Native logic: wide call mismatch cycle=18446744073709551615 got=0x2eaa7,0x1
Native logic: wide call mismatch cycle=18446744073709551615 got=0x1,0x1
Native logic: wide call mismatch cycle=18446744073709551615 got=0x1,0xffffff0000000001
```

`byte-adder` 本身仍然 PASS（额外参数只做校验、不参与运算），所以这是干净的结论：
**四个参数是 JIT 的真实上限**，`(token, cycle, payloadLo, payloadHi)` 不能再扩。

### 取舍（2026-09-22，用户决定）

游戏原版自己的信号上限就是 **64 位**，所以接口按下面这个取舍收口：

1. **每脚 1–64 位**，与游戏一致；**不做多字（>64 位）信号**。这一条直接删掉 `TCComponentTypeDefinitionV2`
   里最贵的那部分（字节视图、宿主借用缓冲、按字节提交）。
2. **总输入预算固定 128 位**（两个 payload 字），因为外调只有 4 个参数。
   于是"脚数"可以放宽（>8 脚仍可做），但**脚数 × 位宽之和必须 ≤128 位**。
3. 代价说清楚：像"64 位加法器（1+64+64 = 129 位）"这种元件**不能**用回调接口直接做。
   它的出口不是改外调形状，而是 M2 的"宿主状态/缓冲"——输出侧现在就已经在用同样的思路
   （`0x9a0000` 状态槽按位回读），输入侧将来照做即可。
4. 这条取舍同时**解锁 V2 的值模型**：每脚一个 `uint64_t` + 宿主借用的脚数组（脚数可 >8），
   不需要字节长度视图；`TCLogicIO`（V1，固定 8 脚）保持不变，V2 走新结构。

### >8 脚：游戏侧的答案（2026-09-22）

探针在 `tc_mod_load` 末尾直接构造并导入 9 / 16 / 32 个输入脚、1 个输出脚的 v14 定义
（字节布局与加载器脚手架一致，只去掉八脚那道门槛；先导入成功再往下走，因为被拒的导入会把
游戏的错误标志留在置位状态）：

```text
pin-shape: wide import 9in/1out  -> status 0 id=6289633417743040512
pin-shape: wide import 16in/1out -> status 0 id=6289633417743040513
pin-shape: wide import 32in/1out -> status 0 id=6289633417743040514
```

`status 0` 是 `TCComponentStatus::Ok`，三个 ID 各不相同——**导入器本身没有八脚上限**，
至少到 32 脚都收。因此 `>8 脚` 的剩余工作全在加载器这一侧：

1. `kMaxBridgeInputs` 与 `Emission::Instance::widths` 等数组放大（纯数组长度问题）；
2. 依赖链长度：脚手架是 `n-1` 个依赖门，n=32 时 31 个门——由游戏编译，导入已证明可行；
3. **真正的硬约束是打包**：输入仍塞进 `token, cycle, payloadLo, payloadHi` 四个参数，
   合计 128 位；`<=8 脚` 时按位宽打包够用，超过 128 位就要么改外调形状（要测 JIT 的寄存器
   分配上限），要么把输入搬到宿主缓冲——这正是 V2 定义结构体必须先看到答案的那一条。

## 复现

```powershell
cd D:\p\tc-modloader
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/pin-shape-playtest.ps1 -Shape source
powershell -NoProfile -ExecutionPolicy Bypass -File tests/pin-shape-playtest.ps1 -Shape sink
```

两个用例都以 `PASS` 结束，并把上面引用的日志行打印出来。

## 引脚锚点是固定的：改 footprint 不会移动它（2026-09-25）

时钟元件按官方常量放大时，为了"包住"更大的本体把 `set_footprint` 从 `2.0 × 1.25` 提到
`2.5 × 1.5`、再提到 `3.5 × 1.5`，沙盒报告里元件的引脚始终是同一行：

```text
sandbox-sim: custom 1 id=0x434c4f4b5f303031 at (0,0) rot=0 pins=0in/1out out0=(2,0)
```

即**生成元件的脚固定在 `(±2, 0/±1/…)`，与 footprint 无关**；footprint 只决定命中、选中、
拖动与占位。半宽 3.5 的后果很直接：元件右侧多出 1.5 格被占住却什么都不画的空间，而且**从引脚
拉线会被判成拖动元件**（玩家原话："footprint 过大了，把输出引脚包裹了"）。

结论与现在的自动告警写在 [../sdk/services.md](../sdk/services.md) 的「声明 footprint 的边界：
不要越过引脚」一节；判据 `component_geometry::footprintReachesPins` 有单测，
`tests/clock-period.cpp` 里另有元件侧的不变量（本体必须在 footprint 内、右侧留出 0.45 格引脚
通道、半宽不超过 2.0）。

想让本体比 2.0 半宽更宽时，**不要去动 footprint**：本征做法是把本体画在框内、右缘停在引脚前
（时钟现在就是 `x ∈ [-2.0, 1.55]`、高 2.93），或者给元件自己缩短/换形。要真正把引脚挪到 `+3`
（常量那样）则必须改生成电路的内部 IO 位置，实测那样会打乱生成走线、板子直接跑不起来
（`-Circuit clock` 用例 nets 全 0），所以没有走这条路。

## 引脚通道现在是**按类型声明**的（2026-09-25：浮点元件要原版尺寸的本体）

浮点元件（`local.float-ops`）要的是"原版 Constant 那样"的外观：本体 4.92 × 2.93 格、名字在右上、
位宽框在左上、**引脚在本体外面**。原版 Constant / Static Value 的 kind 表条目就是 `out0=(3,0)`
（`build/kinds.txt`），所以在 2.0 通道上，引脚必然落在这个本体**里面**——玩家截图里那个压在
本体右缘上的红点就是它。

这一轮先把"设计坐标 → 板面引脚偏移"这条换算量清楚，再把通道做成**每类型可选**的声明，而不是把
所有生成元件的脚一律挪走（那会改掉既有 Mod 的存档连线与元件缩略图）：

```text
板面引脚偏移 = round(电路坐标 / 8)      例：-18 -> -2，+13 -> +2，-4 -> 0，+4 -> +1
1 板面格     = 8 电路单位
```

证据：加载器脚手架的输入脚在电路 `x=-18`、输出脚在 `x=13`，实测注册日志分别是 `in0=(-2,…)`、
`out0=(2,…)`；三脚定义的左列间距 8（`(-18,-10)`, `(-18,-2)`, `(-18,6)`）实测落在 `-1/0/+1` 行
（§12.6）。把通道声明成 3.0 后，同一条日志变成 `out0=(3,0,w32)`、
`in0=(-3,0,w32) in1=(-3,1,w32)`。

接口（`sdk/tc_service_api.h`，都是**追加字段**，`size` 短于它的定义按旧行为处理）：

| 位置 | 字段 | 说明 |
|---|---|---|
| `TCComponentTypeDefinitionV2` | `float pin_lane` | 该类型生成引脚所在的板面半宽（格）；`0` 或未提供 = 历史的 `2.0`；合法范围 `1.0–16.0`，越界直接拒绝注册。写进脚手架时按 `8 × lane` 电路单位取整放置，所以量化到 1/8 格 |
| `TCComponentRenderFrameV1.draw` | `TCComponentRenderDrawV2` | 绘制表追加 `text_sized` / `measure_text`（见 [../sdk/services.md](../sdk/services.md)），让元件能按板面格数定字号，不必猜"板面文字多大" |

**走线必须跟着引脚走。** 上一节那次失败正是因为只挪了引脚、没挪接到引脚上的那根线（`-15` 处的
输入线、`10` 处的输出线）。脚手架的编码器现在把两段线的端点算在引脚自己的连接点上（输入脚
`+3`、输出脚 `-3`）：`wire({{inputPinX + 3, y}, {-7, y}})`、`wire({{6, y}, {outputPinX - 3, y}})`。

真机证据（`tests/float-ops-playtest.ps1`，见 [../verification.md](../verification.md)）：

```text
Native logic: registered custom 0x463332434f4e5331 … out0=(3,0,w32)
Native logic: registered custom 0x4633324144445f31 in0=(-3,0,w32) in1=(-3,1,w32) out0=(3,0,w32) out1=(3,1,w5)
Native logic: registered custom 0x4633324449535031 in0=(-3,0,w32)
PASS float-ops render … body=4.92x2.93 cells … pin outside by 0.55 cells
```

同一个用例仍然跑完 M2 的算术闭环（`4.75` 与半 ULP 用例），说明重新布线后的内部电路照常编译运行；
默认通道上**引脚**不变（`encode()` 在 `pin_lane == 2.0` 时沿用 `-18 / +13` 这组历史坐标），
时钟、文本框等包不受影响。（同一轮之后又把生成电路的内部节点收进引脚矩形，见下一节：那一改只
动内部结构与那张内部图，引脚布置不动。）

## 生成电路的节点位置也是"外观"（2026-09-25 晚：玩家报"FP32 Add 右下角多了一块"）

**现象**：玩家在 FP32 Add 的右下方看到多出一个方块，并猜"游戏识别到这个元件内部右下角有什么
元件然后自动绘制"。这个猜测是对的，量出来的数字也能对上：以引脚定标（±3 格在图上相距 150 px
→ 25 px/格），方块中心在元件中心右 6.6 格、下 13.8 格 = 电路坐标 `(52.6, 110)`，而旧脚手架
的依赖门就在 `(40 + 12·1, 100 + 8·1) = (52, 108)`。

**机制**：游戏为每个自定义元件渲染一张"内部电路"图（`asset/capture/com_custom_<id>.png`
这样的文件；随包的示例里能直接看到它的样子——内部逻辑若干色块 + 两侧引脚点，透明底），
元件栏、元件工坊与预览都用它。所以**脚手架节点摆在哪里，玩家就会在那些画面里看到什么**：
旧布局把依赖门摆在离元件中心 13 格之外，那张图上就多出一块。

**结论（现在的约定）**：生成电路的所有节点必须落在**引脚自己的矩形**内。三列位置固定在
`src/component_definition.hpp`：

```text
输入收集门  x = -6   （与各输入行同高）
依赖门      x =  0   （与输入行同高，2 输入门自己的脚在 (-1,-1)/(-1,+1)）
输出驱动门  x =  6   （与各输出行同高）
```

依赖链的接线随之重画：链沿依赖门左一列（`x=-3`）上下走，每个输入从收集门的输出点
（`x=-4`）直接接到门的第二个脚，链尾从最后一个门的输出走到第一个驱动门的输入。节点**种类与
顺序**是契约，坐标不是——`tests/native-component.cpp` 现在逐节点断言它们不越界（两脚一侧、
16 脚两种形状），并核对九种节点的顺序。

这次的改动**不动引脚**（引脚列与行都没变），所以既有存档的连线不受影响；上一节那次"引脚从
±2 挪到 ±3"才需要玩家重接一次线。
