> **研究/交接日志（非发布文档）。** 本文记录"让 simcore 兼容原版全部内置元件"这件事
> 做到哪一步、每一步的证据在哪、剩下的为什么不能直接写。面向使用者的结论请从
> [文档索引](../README.md) 进入。

# simcore 对原版内置元件的覆盖：现状与测量清单

日期：2026-09-29。构建：`Turing Complete.exe`（该指定构建）+ 本仓库的 `simcore`。

盘点工具：`python tools/kind-coverage.py`（读 `build/kinds.txt` 与
`simcore/include/tcsim/board.hpp` 的表，输出下面第一张表里的数字）。

## 1. 一句话结论

`build/kinds.txt` 记的 **109 个内置原型**里，**108 个已经能进 simcore 的网表**（16 个门级
器件 + 27 个字宽/叶子/索引/接口/译码器件 + 47 个 `board.hpp` 手写分支 + 18 个原型表里
0 进 0 出的空槽），**1 个没有**：`0x50`，一个没有名字、引脚偏移是哨兵值 `32767` 的槽位——
连**游戏自己都拒绝放置它**（`tests/kind-oracle-playtest.ps1 -Kind 0x50` 返回
`place state=4 result=-6`），所以它不可能出现在任何棋盘上，按"识别并明确报未支持"交付。
`python tools/kind-coverage.py` 打印这张表。

§7 记的是最要紧的那一族：**寄存器、计数器、延迟线**（以及它们的位宽版和 Auto 版）。它们的
输入脚根本不在原型表的声明列表里，而记录**算**这两个脚，所以在这之前任何带寄存器的板子都会
被整块拒绝（真机原话：`component 3 (Counter) has 2/1 pins in its record but the prototype has
0/1`）。

**离线测量工具是这一轮的主力**：`tools/netlist-view.py` 直接把游戏自己的
`hint_*.data` / `hint_solution.data` / `hint_custom/**/circuit.data` 读成网表——引脚
来自游戏的原型表（`build/kinds.txt` 自动解析，125 个 kind），连线来自文件本身。
`--scan` 还会统计"每个 kind 被用了几次、它的每根脚到底有没有接线"，后面几处结论都是
它给出来的。

## 2. 覆盖表

| 类别 | 数量 | 说明 |
|---|---|---|
| 门级器件（`devices.hpp` 的 `deviceNameForKind`） | 16 | NOT/AND/NAND/OR/NOR/XOR/XNOR 的位与字两套 kind |
| 字宽运算器件（第一刀） | 8 | `kinds.txt` 里 `wauto` 的二入一出运算，见 §3.1 |
| 位序/拼接器件（第二刀） | 14 | 全部 Splitter / Maker / Concatenator，见 §3.2 |
| Static Indexer（第三刀） | 1 | `0x65`，取字段的规则来自游戏自己的 ISA，见 §3.3 |
| 接口引脚（第四刀） | 2 | `0x4f` Input Pin 与 `0x51` Output Pin，见 §3.4 |
| **译码器三件（第五刀）** | **3** | `0x2b` / `0x2c` / `0x2d`，一位热码，见 §3.5 |
| `board.hpp` 手写分支 | 47 | 常量、输入/输出脚、探针、Switch/Mux、RAM、Load/Store 端口、屏幕、Time/Keyboard，以及第七刀的存储家族与关卡件 |
| 原型表里的空槽（0 进 0 出且无名） | 18 | `0x42/0x43/0x47/0x48/0x4c/0x56/0x58/0x66/0x67/0x69/0x6b/0x71/0x78–0x7c` 等，本构建里没有实体 |
| **仍未支持** | **1** | `0x50`：游戏自己拒绝放置（§4 表末） |

## 3. 本轮做了什么（每一条都有依据）

### 3.1 字宽运算族（`devices.hpp`）

新增 `WordOp` 与三个构造器，覆盖 `kinds.txt` 里**声明了 `wauto` 的二入一出运算**：

| kind | 名字 | 器件 | 依据 |
|---|---|---|---|
| 0x1a | Equal | `equalW` | 原型表 `in0/in1=wauto out0=w1`；`campaign/byte_mux/test.si` 用 `==` 描述 |
| 0x1b / 0x1c | Low < (unsigned) / Less < (signed) | `lessU` / `lessS` | 同级关卡 `byte_less_u`、`byte_less_s` 各自的名字与 `.si` |
| 0x1e | Add | `addW`（带进位入/出） | 原型表 `in0=(-1,-1) in1=(-1,1) in2=(0,-2,w1) out0=(2,0) out1=(1,2,w1)` |
| 0x1f / 0x20 / 0x6c | Multiply / Divide / Modulo | `mulW` / `divW` / `modW` | 同级关卡 `multiply`、`byte_divide`、`byte_mod`；`byte_divide` 的测试说明"除零"的答案 |
| 0x21–0x25 | 左移/右移/循环左/循环右/算术右移 | `shlW` / `shrW` / `rolW` / `rorW` / `asrW` | 关卡 `byte_lsr`、`byte_asr` 的名字；原型表五者引脚同形 |
| 0x13–0x18 | 字宽 OR/AND/NAND/NOR/XOR/XNOR | `orW`/`andW`/`nandW`/`norW`/`xorW`/`xnorW` | 原型表：与 0x07–0x0b 的位级同名件成对，字宽件声明 `wauto` |
| 0x12 / 0x1d / 0x31 / 0x39 / 0x68 | 字宽 NOT / Negate / 前导零 / 后导零 / Increment | `notW`/`negateW`/`clzW`/`ctzW`/`incW` | 同上；`count_leading_zeroes`、`byte_not` 是同级关卡 |
| 0x72 | Concatenator（2 入） | `concat2` | 原型表把顺序写死：`in0=(-1,-1,w2) in1=(-1,0,w2) out0=(1,0,w4)`，in0 在高位 |

**四态规则（本轮唯一一处"按游戏而不是按 Verilog"的决定）**：游戏生成的程序是
**64 位字上的普通 C**，而它自己的参考仿真器把每个值都解析成具体数，所以字宽运算里
**任意一个未定（X/Z）操作数位会把整个结果变成 X**，而不是只污染它自己那一列。
simcore 照这个来；比较类因为答案只有一位，未定操作数给一个 X。位级门（`deviceNot`、
`logicGate`）仍保留逐位真值——那是门电路物理上的行为，游戏对它们也是逐位编译的。
理由写在 `devices.hpp` 该族的说明里：一个游戏不认的"更精确"答案会让交叉验证说谎。

### 3.2 位序/拼接族（`devices.hpp` 的 `deviceSplitter` / `deviceMaker`，第二刀）

`board.hpp` 新增 `leafKindSpec` 表，把 14 个 kind 接上既有的两个器件：

| kind | 名字 | 宽度来源 |
|---|---|---|
| 0x11 | Splitter（8 出） | 声明 `in=w8`，与输出现数一致 |
| 0x6e / 0x6d | Splitter（4 出 / 2 出） | 声明 `in=w4` / `w2` |
| 0x63 / 0x64 | Splitter（4 出 / 8 出） | 输入 `wauto`，见 §3.7 的实测（输入 = 叶子数 × 每片宽度） |
| 0x2f | Splitter（2 出） | 输入脚是三个宽哨兵之一（`…fe`），见 §3.7 的实测 |
| 0x10 / 0x70 / 0x6f | Maker（8/4/2 个一位叶） | 输出 `w8`/`w4`/`w2` |
| 0x62 / 0x61 / 0x30 | Word Maker（8/4/2 入） | 各叶 `wauto`，见 §3.7 的实测（输出 = 各输入宽度之和） |
| 0x73 / 0x74 | Concatenator（4 入 / 8 入） | 同 Word Maker：输出 = 各输入宽度之和 |

**位序约定（本文这一版唯一一处"两可之间做选择"）**：原型表给出每根叶子的**偏移**
（`(1,-1)` 起自上而下）与原始宽度，但**没有说最上面那根叶子是最高位还是最低位**。
本版取"**最上面的叶子是最高位**"，理由三条：①它让 Splitter 与 Maker 互为精确逆运算，
而 `bus_split` 电路已经在 `simcore` 与 Icarus Verilog 之间逐网对过；②这与 2 入
Concatenator 由声明宽度强制的读法一致（`0x72` 的 `in0` 在高位，关卡就是用它拼字的）；
③这是**命名**层面的选择，不是发明行为——两种读法都满足"输入什么、输出什么"，
只有"拆出来单独喂给别处"的板子才看得出差别。

`0x65` 的规则已经量到（§3.3），`0x2f` 那一族的**宽度**也量到了（§3.7）；只留下
"叶子单独使用"的读法仍按上表这条约定办事——两种读法都满足"输入什么、输出什么"，
只有"拆出来单独喂给别处"的板子才看得出差别，而那需要一块这样的真机板子。

### 3.7 两族 `wauto` 的宽度：从玩家自己的板子上量（2026-09-30）

原型表把"自动宽度分线器"（`0x2f` / `0x63` / `0x64`）与"Word Maker / 宽 Concatenator"
（`0x30` / `0x61` / `0x62` / `0x73` / `0x74`）的数据通路写成哨兵，所以表本身不说宽度。
游戏自带的 217 份电路里这两族**一次都没出现**（`python tools/netlist-view.py --scan
D:\p\campaign --kinds 0x30 0x61 0x62 0x63 0x64 0x2f` 全部 0 次），所以只能量真的板子：

```text
$ python tools/kind-wire-widths.py <sandbox-sim-board.txt> --kinds 0x2f 0x30
kind 0x2F (Splitter) at 138,-2 r1
  in0 (138,-3) declared=auto  wire(s): 64
  out0 (139,-1) declared=auto  wire(s): 32
  out1 (138,-1) declared=auto  wire(s): 32
kind 0x30 (Word Maker) at -10,37 r0
  in0 (-11,36) declared=auto  wire(s): 8
  in1 (-11,37) declared=auto  wire(s): 8
  out0 (-9,37) declared=auto  wire(s): 16
```

记录的 `+0xe0` 独立印证同一件事：那颗 `0x2f` 写的是 **32**（每片宽度），那颗 `0x30`
写的是 **16**（输出总宽）。于是规则是：

| 族 | 规则 | 依据 |
|---|---|---|
| Splitter `0x2f`/`0x63`/`0x64` | 输入 = 叶子数 × n，每片 n 位；n 依次取"任一出脚上那根线的宽度"、"输入宽度 ÷ 叶子数"、"记录 `+0xe0`" | 64 → 32+32；记录 32 |
| Word Maker `0x30`/`0x61`/`0x62`、宽 Concatenator `0x73`/`0x74` | 每根输入保留**自己那根线**的宽度，输出 = 各输入宽度之和（最上面的输入是最高位） | 8+8 → 16；记录 16 |

器件是 `deviceWordSplitter(leaves, n)` 与 `deviceWordMaker(widths)`（位序沿用 §3.2 那条
约定：最上面的叶子是最高位）。门槛：`testAutoWidthSplitterAndMaker()` 用"8+8 → 16"、
"16 → 4×4"两块合成板钉住解析顺序，并直接对器件的真值表断言（`10101011`+`11001101`
→ `1010101111001101`）。真机结果：玩家那块 52 元件、7 份定义、带一颗 Word Maker 的板子
从"有一件被拒绝并报 note"变成**全部建成**（`u156_wordmaker`、`u152_wordsplit2x32`），
窗口读同一份记录得到的 note 与游戏内 report 一字不差。

### 3.3 Static Indexer（`0x65`）：规则来自游戏自己的 ISA（第三刀）

`0x65` 取字段，配置就在元件记录里：`values[0]` 是**带符号的 index**，`+0x70` 是**输出
位宽**（原型表那行 `raw[…]` 里的 `112:<n>` 就是它）。规则本身不是猜的，是**游戏自己的
两份文件对齐出来的**：

| 证据 | 内容 |
|---|---|
| `campaign/symphony_1_nand/test.si` 第 31–37 行 | 关卡自己这样解码：`mode = (instruction >> 29) & 0b11`、`opcode = (instruction >> 24) & 0b1111`、`dest = (instruction >> 20) & 0b1111`、`a = (instruction >> 16) & 0b1111`、`b = (instruction >> 8) & 0b1111`、立即数标志在 28 位、立即数 = `instruction & 0xffff` |
| 同一关的 `hint_custom/Instruction Decoder/circuit.data`（游戏自己的解） | 七个 Static Indexer 配置成 `(29,2) (24,4) (20,4) (16,4) (8,4) (28,1) (0,16)`——**与 ISA 同一组数、同一组宽度** |
| `campaign/multiply/hint_solution.data` | 六个 Static Indexer 用**负** index（`-1`…`-7`，宽度 8）搭出一张表；该关自己的测试是 `(a * b) & 0xff`，也就是说负 index 是**左移** |

于是规则是：**index ≥ 0 时 `(A >> index) & ((1<<width)-1)`；index < 0 时向另一方向移位，
空出的低位补 0**。唯一"由关卡暗示而不是明说"的一处是负 index 是否也套同一个掩码（本版
按方向移位实现），已写进 `deviceIndexer` 的注释与 §5 的待测清单。

**顺带钉住的第二件事**：那条解还把"**最上面的叶子是最高位**"这条约定往上顶了一格——
Instruction 是 32 位，Splitter 的 8 根叶子接七个 Output Pin（`0x51`），
`out0` 那一片接的 indexer 恰好是 `(24,4)`，也就是"最高位那一片承载 bit 24–27"。
若按"最上面是最低位"读，同一个解就会把 opcode 接到 `(0,16)` 上，与 ISA 直接矛盾。
所以 §3.2 的默认值在**关卡自己的解 + 关卡自己的 ISA** 两处同时成立；真机那一次对照
（§5）仍然要做，因为它能给出直接的读数而不是一致性论证。

### 3.4 接口引脚：`0x4f` Input Pin 与 `0x51` Output Pin（第四刀）

它们是自定义元件的两个界面脚，本版实现为**直通**。证据是**结构性的、覆盖全部 217 份
游戏电路**的（`python tools/netlist-view.py --scan --kinds 0x4f 0x51`）：

| kind | 游戏电路里的用法 | 份数 |
|---|---|---|
| `0x51` Output Pin | **每一份**的 `in0` 都接着线、`out0` 都不接线 | 90 / 90 |
| `0x4f` Input Pin | **每一份**的 `in0` 都不接线、`out0` 都接着线 | 74 / 74 |

直通件正是这个样子：数据脚接、边界脚落在元件自己的格子上（Input Pin 的 `in0=(0,0)`、
Output Pin 的 `out0=(0,0)`，都是**边界**那一格，所以复合元件外部的连线不会出现在这份
电路文件里）。反过来，源件会"输出接线、输入不接"，汇件会"输入接线、输出不接"——两者
都对不上这张表（`0x51` 的 `in0` 是**有人喂**的，`0x4f` 的 `out0` 是**有人读**的）。

于是实现是：一个宽度可变的直通器件（`devicePin`），宽度由它承载的那根导线决定；
**没有任何东西驱动它时输出 Z**（高阻），而不是"某个未知值"——这也是"没接"的诚实答案。
真机对照（把一个 Input Pin 的输出读到棋盘上）仍列在 §5，因为这份证据是结构性的。

### 3.5 译码器三件：`0x2b` / `0x2c` / `0x2d`（第五刀）

原型表把这三件的形状写全了（`0x2b`：`in0=(-1,0,w1)`、两个 `w1` 输出；`0x2c`：
`in0=(-1,-1,w2)`、四个 `w1` 输出；`0x2d`：`in0=(-1,-2,w3)` + `in1=(0,-4,w1)`、八个
`w1` 输出），所以宽度不需要从导线推。行为是一位热码：输出 `k` 为 1 当且仅当地址等于 `k`。

- **行为的形状**有证据：`campaign/overture_2_alu` 与 `overture_3_immediates` 的解把
  3 Bit Decoder 的 `out0..out2` 接进一个 4 输入 Maker 的**高三个叶子**、把一个 Splitter
  的 `out2` 接进最低叶子——那就是一个四选一的选择字。
- **`in1` 是使能（推断，未实测）**：它是这三件里唯一一个声明偏移不在数据通路上的脚
  （`(0,-4)`，比地址脚高一行），在 11 次出现里只接了 1 次，且那一次承载一位信号；不接时
  保持使能（玩家看到的就是"放着就能用"）。极性（高有效/低有效）与"不接线时到底是不是
  使能"都要真机读数，已列进 §5。

### 3.6 板级接入（`board.hpp`）

- 新增 `wordKindSpec` / `wordUnarySpec` / `isConcatKind` 三张表，把 kind 映射到器件名；
- 自动宽度从"每元件一个数"改成**每引脚一个数**：移位/循环的**移位量是参数脚**，
  宽度取到达它的那根导线，而数据脚取整条数据通路的宽度。权威顺序是
  ①游戏自己写进记录 `+0xe0` 的 `memory_bits`、②数据脚上最宽的那根导线、
  ③原型表声明的宽度；三者都没有时**报"宽度未确定"**，不给它一个 1 位的默认器件；
- 新增 `kindHasAutoDataWidth` / `kindHasAutoRightWidth`，字宽件与 Switch/Mux 走同一条宽度通路。

### 3.3 离线门槛

`simcore/tests/simcore-tests.cpp` 新增 `testWordOps()`（40 条断言）：进位入/出、除零、
有符号与无符号比较、移位量与字宽相等时的边界、循环移位的绕回、四态污染规则、
Concatenator 的高低位、以及"未知名返回空指针而不是近似件"；
另加 `testWordWidthResolution()`（合成板，4 条断言）钉住三条宽度权威顺序，以及
**没有任何宽度来源的字宽件被报出来而不是建成 1 位**；
再加 `testStaticIndexer()`（9 条断言，期望值按位号手算而非取自被测器件）：ISA 那五个
字段的偏移与宽度、越过输入右端的字段读成 0、负 index 的左移、宽度等于字宽时的恒等、
以及未定位让整字段变 X。
`simcore.ps1` **318/318**、`view-tests.ps1` **188/188**、`simcore-iverilog.ps1` **8/8**
（门级电路的逐事件表与 Icarus Verilog 13.0 仍然逐网一致——新代码没有碰到它们）。

本轮被门槛抓住的三个 bug（都已修，写在这里免得下次重犯）：
`switch` 的组合 case 标签被编辑挤掉；字宽按位运算读 `BitVector` 时把位序读反
（向量是 MSB-first，而 C 值的第 0 位是向量的最后一个元素）；"先污染后比较"让比较件
输出 4 位而不是 1 位。另有一处真回归：宽度改成"每引脚一个数"之后 `memory_bits`
会被写进**控制脚**（Mux 选通 / Switch 使能 / 移位量），把它们撑成 32 位。

## 4. 还差的 19 个：先看量到的用法，再看缺什么

下面每个数字都来自 `python tools/netlist-view.py --scan campaign --kinds …`（217 份游戏
自带电路）。"接线索数"是"该脚在多少次出现里真的接着线"，所以它同时告诉我们**哪些脚是
信号脚、哪些是配置脚或边界脚**——这是真机探针要打的第一张靶纸。

| kind | 名字 | 出现 | 接线的脚（接/出现） | 记录里带的东西 |
|---|---|---|---|---|
| `0x27` | Register | **71** | `out0` 33/71 | `bits=8`、`i16` = **-2/-4/-6/-8…**、`boolA=1` |
| `0x6a` | Input | 39 | `out0` 18/39 | `bits=8`、`i16` 负数、`boolA=1` |
| `0x3e` | Switched Input | 20 | `in0` 1/20、`out0` 5/20 | `bits=8` 或 **32**、`i16` 负数 |
| `0x2c` | 2 Bit Decoder | 14 | `in0` 2/14、`out0..3` 8–12/14 | — |
| `0x2d` | 3 Bit Decoder | 11 | `in0` 2/11、**`in1` 1/11**、`out0..3` | — || `0x37` | Delay Line | 11 | `out0` 11/11（**原型表里没有任何脚**） | `bits=32` |
| `0x53` | Memory Probe | 10 | 无（0 进 0 出） | `bits=32`、`boolA=1` |
| `0x26` | Counter | 3 | `out0` 1/3 | `bits=8`、`i16=-14/-2`、`boolA=1` |
| `0x2b` / `0x4d` | 1 Bit Decoder / Counter(3 进) | 1 / 1 | `in0..2` **全都没接线** | `bits=8`、`i16=-6`、`boolA=1` |
| `0x77` `0x32` `0x59` `0x5a` `0x5e` `0x75` `0x57` `0x33` `0x35` `0x3b` `0x29` `0x50` | Auto Delay Line / Auto Register / 7 Segment / Static Value / Static Evaluator / Auto Static Indexer / Halt / Disassembling probe / 无名 / Level Component / Level And Delay / 无名 | **0** | — | — |

**读出来的三件事**：1. **十二个 kind 在游戏自己的电路里一次都没出现**（Auto Register、Auto Delay Line、
   7 Segment、Static Value、Static Evaluator、Auto Static Indexer、Halt、Disassembling
   probe、两个无名槽、Level Component、Level And Delay Component）。它们是编辑器里
   能放、但游戏自己的解不用的件。这一组**优先级最低**，而且其中几个的语义可能根本不由
   棋盘决定（`0x57` Halt 停仿真、`0x59` 只是显示、`0x5e` 要读组件的配置）。
2. **`0x27` Register 的入口不在棋盘上**：71 次出现里 `out0` 只有 33 次接线（其余落在
   复合元件的边界），而它的 `in0` **一次都没接**；记录里 `i16` 是 -2/-4/-6/-8… 这串
   负数、`boolA=1`。`campaign/assembly_programming/circuit.data` 里六个 Register 的名字
   就叫 `Reg 0`…`Reg 5`，同一份电路里还有 `0x26` Counter 叫 `Count`、`0x76` RAM 叫
   `Program`。也就是说**它是靠名字/索引与仿真器打交道的**（寄存器堆由关卡脚本读写），
   棋盘上根本不接输入脚——这正是"必须真机读数、不能从棋盘推"的典型。
3. **`0x3e` Switched Input 的 `in0` 几乎不接**（1/20）而 `out0` 只接 5/20，宽度有 8 和
   32 两种：它更像"由元件自己的配置/状态决定值"的源件，和 `0x6a` 同一类。
4. **原型表里根本没有任何引脚的有两种件**，而它们的原因不同，这决定了下一步该往哪查：

   | 情形 | 例子 | 含义 |
   |---|---|---|
   | **状态件**：端口由板级接入自己建模，不走原型表 | `0x76` RAM（已在 `board.hpp` 里按插座顺序接 Load/Store 端口）、`0x52`/`0x53` Memory Probe（0 进 0 出，值是探针读的内存） | 引脚不是"没量到"，而是**本来就不是引脚**：值在别处 |
   | **真的缺几何**：原型表这一行没有声明任何脚，而棋盘的导线确实落在它旁边 | **`0x37` Word Delay Line、`0x0d` Bit Delay Line** | 这两件在电路里 `out0` 100% 接线（11/11），但表里 `inputs=0 outputs=1`、没有任何 `in…` 行——也就是说**输入端的位置只有游戏自己知道**，`kinds.txt` 这一版 dump 不出来 |

   `0x37` 的记录指纹已经量到了（`bits=32`，与 `0x0d` 的 `bits=1` 成对），所以缺的**只是
   端口几何**：要么在真机上把它的引脚位置读出来（`Board V5` 那套读的是原型表，表里没有
   就得反汇编或让元件自己报），要么先只支持"输出端"这一半。

   **顺带量到一半**：把 217 份电路里所有 Delay Line 的坐标取出来，看导线端点落在哪些相对
   格子上，答案是**恰好两个**——`(-3,0)` 与 `(3,0)`，11 份里各命中 11 次（`(3,0)` 正是
   原型表没写出来的那个 `out0`，与 `0x51` Output Pin 的 `in0=(-3,0)` 同一个偏移）。
   所以几何其实已经量出来了，**真正没量到的是行为**：这一根 `(-3,0)` 上的输入是"下一拍
   出现在输出"还是别的时序。按红线 6 在那之前不建器件。

   这也是本文里第一条"缺的东西不是字段值、而是引脚位置"的项目，写在这里免得下一轮又去
   猜记录字段。

### B. 叶子位序与静态取值（1 个）：缺"哨兵宽度到底是多少"

`0x2f` Splitter（输入脚是宽哨兵 `0x7fff…fe`）。它已经**在表里被认出来**，但 `leafWidth`
会拒绝它并报"not guessed"——它与 §3.2/§3.3 的位序约定是同一组待测量的两面。

### C. 状态件（9 个）：缺"字段语义"与"容量公式"

`0x26` Counter / `0x27` Register（字） / `0x32` Auto Register / `0x37` Delay Line（字） /
`0x77` Auto Delay Line / `0x4d` Counter（3 进 0 出） / `0x29` Level And Delay Component /
`0x3b` Level Component / `0x5a` Static Value

`kinds.txt` 已经指出其中几个的**配置字段就在原型行里**（`raw[0x58..0x80]`）：
`0x32`/`0x77` 的 `112:2`（= `+0x70`）、`0x27`/`0x26` 的 `112:2`、`0x37`/`0x0d` 的 `112:1`。
`+0x70` 这个字段名还不知道，容量/初始值/使能极性都可能是它，**不能猜**。
`0x4d` Counter 的三个输入脚（`in0=(-1,-1) in1=(-1,0) in2=(-1,1)`，0 输出）说明它是个
**只写面板的计数器**，值从哪读也没量。

**要测什么**：对每个 kind 的真机记录逐字段 dump（`TC_SIM_STATE_DEEP=1` 那套已经有），
再把"改这个字段 → 行为怎么变"跑成对照。这一族里**最该先做的是 `0x27` Register**：它在
217 份游戏电路里出现 **71 次**（`0x26` Counter 只有 3 次），而且扫描已经量到它的形状——
71 个里有 33 个"输出接着线"，另外 38 个的输出落在复合元件的边界上；它的 `in0` 在任何
一份里都没有接线，所以那个入口是靠元件自己的记录/状态驱动的，正是最需要真机读数的一处。
`0x37` Delay Line（11 次）量到的记录指纹是 `bits=32`（`0x0d` Bit Delay Line 是 `bits=1`），
但它的**引脚几何在原型表里是空的**——见上面第 4 条，缺的不是字段值而是端口位置。### C. 译码器（3 个）：缺"位序与选通脚"

`0x2b` 1 Bit Decoder / `0x2c` 2 Bit Decoder / `0x2d` 3 Bit Decoder。
原型表里 `0x2c` 的输入脚在 `(-1,-1)`、输出在 `(1,-1)…(1,2)`，而 `0x2d` 有第二个输入脚
`(0,-4,w1)`——**同一个"译码器"族里三行的形状互不相同**，说明至少有一行不是
"输入在左、输出在右"的常规形状。在量到之前不写。

### D. 探针与脚（8 个）：多半是直通，但要确认"有没有副作用"

`0x33` Disassembling probe / `0x4f` Input Pin / `0x50` / `0x51` Output Pin /
`0x53` Memory Probe / `0x57` Halt / `0x59` 7 Segment Display / `0x35`（无名，0 进 2 出）

`0x57` Halt 会**停掉仿真**，`0x33` 反汇编探针会**读组件自己的配置**——这两个不是直通，
必须实测；其余几个确认是直通即可。

### E. 其他（4 个）：`0x3e` Switched Input / `0x5e` Static Evaluator /
`0x6a` Input（宽度在记录里） / `0x75` Auto Static Indexer

`0x6a` 的输出脚是 `wauto` 而它**没有输入脚**，宽度只能来自记录里的某个字段——
这正是 `board.hpp` 里 `data_width == 0` 时会报"宽度未确定"的那一类。

## 5. 一次性测量：`kind-oracle` 探针（装置可用；行为测量受阻，见 §5.20）

> 本节按时间顺序记录整个过程（第 7–28 轮），包括**被推翻的假设**与**我自己的错误**。
> 只看结论的读者可跳到 **§5.13（能力清单）** 与 **§5.20（阻塞判定）**。探针本体在
> `tests/kind-oracle-probe.cpp`，跑法见 `tests/kind-oracle-playtest.ps1`。

## 5.1 行为那一半怎么做（最初的路线设计）
**已经写好的**（本轮）：

| 文件 | 内容 |
|---|---|
| `tests/kind-oracle-probe.cpp` | 原生探针：等棋盘 → 用 `tc.commands` 的 `PLACE_COMPONENT` 放**一个内置 kind**（`validateCommand` 明确允许内置 kind）→ 用 `tc.board` V3/V4 把**每个元件记录与每根导线记录（含 `state_byte_offset`）**写进 `<插件数据目录>/board-dump.txt`。配置走环境变量 `TC_KIND_ORACLE_KIND/X/Y/ROTATION`，只读、不驱动仿真。已用仓库的 MinGW 编译成 `build/kind-oracle-probe.dll`（`-Wall -Wextra` 无警告）。 |
| `tests/kind-oracle-playtest.ps1` | 隔离沙盒（复制游戏 + 加载器 + 探针包 + `tcmod-cli apply`），设好四个环境变量后启动，等 `result.txt`，断言 `PASS`，并把 dump 抄到 `build/kind-oracle-<kind>/board-dump.txt`。脚本已通过 PowerShell 解析检查。 |

**跑起来之后量到的（真机部分，本轮首次拿到读数）**：装置端到端通了——沙盒起得来、包
装得上、探针进得了棋盘、`result.txt` 被脚本读到并断言。跨过去的四道坎（每一条都是真机
报出来的，不是推的）：

1. **加载器只允许一个 `igInvisibleButton` 钩子**（同一函数挂第二个被拒）。所以探针不自己
   挂钩，直接复用 `enter-board.cpp` 的加载器，只把 `on_frame` 指回自己的帧回调。
2. **`captureBoardObjects` 的 `buffers` 不是可选的**：传 `nullptr` 会被判 `ARGUMENT(-2)`，
   而且 `buffers->version` 必须等于 `TC_BOARD_OBJECT_SNAPSHOT_VERSION_1`——我第一版两次都
   踩了（`version` 忘了设、首调还传了空指针）。正解是**带上第一版猜测的缓冲调两次**，
   第一次的失败会把真实数量写回 `out`，再按它扩容重试。
3. **只能读 `written` 个句柄**：缓冲容量比实际对象多时，越界那些句柄读出来是"unreadable"
   （第一版 dump 里出现了 index 2..63 全是 unreadable）。
4. **内置 kind 真的能通过命令总线放上去**，而且放上去之后能读回记录。

**四份真机读数**（隔离沙盒，各一次，`build/kind-oracle-<kind>/board-dump.txt`）：

```text
PASS kind-oracle kind=0x2c x=0 y=0 rotation=0
board frame=340 generation=343 components=2 wires=0 written=2/0
component index=0 kind=0x00 x=0 y=0 rotation=0 id=0
component index=1 kind=0x2c x=0 y=0 rotation=0 id=811146521334999133
```

同样的形状在 `0x27` Register、`0x6a` Input、`0x3e` Switched Input 上各复现一次（各自拿到
独立的非零实例 id）。index 0 的 `kind=0x00 id=0` 是那块棋盘自己的记录，与
`board.hpp` 里"kind 0 is the board's own record"一致。

**这四份读数量到的是"能放、能读、能给 id"，还不是行为**——行为要下一步把值驱动进去、
跑周期、再读状态数组（`wire.slot` 指向的字节）。

**第五份读数（带坐标与旋转）**：`-Kind 0x2c -X 7 -Y -5 -Rotation 1` 读回
`component index=1 kind=0x2c x=7 y=-5 rotation=1`——坐标与旋转是照请求落下的，所以这套
装置可以直接用来量"某个 kind 在某个坐标/旋转下的记录与引脚"。探针在放置后还会等 5 帧
让游戏把记录安定下来再读。

### 5.1b 细节（原「下一轮的具体设计」）

难点不是驱动，而是**引脚在棋盘上的位置**：`kinds.txt` 的声明里 `0x37` Delay Line 这一类
是空的，而"输出落在哪个 `state_byte_offset`"只有游戏自己知道。三条路，按风险从低到高：

1. **把值写进状态数组再看游戏怎么读**（最省事，但需要槽位号）；
2. **替游戏写导线记录**：记录布局已经实测过（`+0x18..0x1e` 两个端点、`+0x30` 宽度、
   `+0x38` 状态偏移），板级接入 `src/board_edits.hpp` 就是照模板写的元件记录；导线要自己
   造一条记录并让游戏的 `update_wire` 接手，风险与当年"放元件不带 registration"同类；
3. **用关卡内容当模板**（最安全）：`tests/level-content-playtest.ps1` 已经证明可以往
   `campaign/<关卡>/circuit.data` 里放**自己写的电路**并让游戏加载运行，而 circuit v13/14
   的编码器就在 `tools/circuit_format.py`。把目标 kind 与常量/输出脚写进一份 v14 电路、
   部署成关卡、进关跑周期、读状态数组——这条路**完全不碰游戏内存**，只是把电路写成文件。

按红线 6 的取向，**优先走第 3 条**：它是"游戏自己加载并运行我们写的电路"，量到的是真机
行为而不是我们模拟的行为。第 1、2 条留作第 3 条走不通时的备选，并且要先有真机读数支持。

### 5.2 第 3 条的落脚点（本轮探明）

- **编码器可用**：`tools/circuit_format.py` 的 v13/14 编解码**往返通过**（读一份游戏电路、
  再编码、再解码，元件与导线数量一致），`python-snappy` 在本机可用。所以"写一份自己的电路
  文件"这件事没有工具障碍。
- **模板已经找到**：`campaign/overture_decoder/hint_solution.data` 就是一份**最小的译码器
  电路**——`0x6a Input(Instruction)` → `0x11 Splitter` → `0x2c 2 Bit Decoder` → 四个
  `0x3a Output`，再加一个 `0x6f Maker`。把它的名字与输入换成我们要的，就能得到一份"游戏
  自己的解"级别的电路，不需要从零构造元件记录。
- **新增 `tools/pin-slots.py`**：把一份关卡电路里"哪根导线落在哪个元件的哪个脚上"打出来。
  它已经在这份解上验证了自己——四个输出脚各自引出一根导线到四个独立的 Output：
  `c1:2 Bit Decoder.out0 → c4:Output`、`out1 → c5`、`out2 → c6`、`out3 → c7`，
  输入侧 `in0` 接 `0x11 Splitter`、Splitter 再吃 `0x6a Input`。**这就是一位热码译码器在
  真机电路里的形状**，与 simcore 的实现一致（但要拿到"读数"还得跑下一步）。
- **同时探明一条限制**：**关卡电路里没有状态偏移**（`pin-slots.py` 的提示就是这件事）——
  偏移是游戏加载时分配的。所以要么在真机上把分配结果读回来（`board-dump.txt` 里每条
  wire 记录都带 `state_byte_offset`，这也是为什么探针要 dump 导线），要么在关卡里放
  Output 脚、直接用游戏自己的输出把值显示出来。

**下一轮的具体动作**：用 `overture_decoder` 的解当模板生成一份 v14 电路（把 `0x6a Input`
换成一个常量、把四个 Output 保留），写进隔离沙盒的 `campaign/sandbox/circuit.data`，
进关卡跑几个周期，再从 `board-dump.txt` 的导线记录里读出四个输出脚的状态偏移与值。
那一步拿到的就是**第一份行为层面的真机读数**。

### 5.3 生成器已落地（本轮）

`tools/level-circuit.py` 写出了上面那份电路。它**不改电路的形状**——上一版想"只保留某些
kind 再删掉多余导线"，实测发现那份解把多根导线接在同一格上（总线式共享），按"两端都在
保留的引脚格上"过滤会把 14 根导线全丢光；保留游戏原本的画法才是安全做法。所以现在的动作
是：读模板 → （可选）给关卡自己的 Input 改个名字/宽度 → 编码写回。实测：

```text
python tools/level-circuit.py ../campaign/overture_decoder/hint_solution.data \
    --rename-input Instruction --outdir build/decoder-level
  -> renamed 1 level input component(s) to 'Instruction'
  -> copied sandbox's meta.txt/label.txt into build/decoder-level
再读回来：components 8 wires 14 version 13，Input 的名字是 'Instruction'
```

也就是说**"写一份能部署、能被解析器读回的关卡电路"这件事已经完成**，剩下的就是把这份
`circuit.data` 放进隔离沙盒的 `campaign/sandbox/`、进关卡、跑周期、读导线记录。

### 5.4 第一份行为层面的真机读数（本轮）

上面那三步已经跑通。命令：

```text
python tools/level-circuit.py ../campaign/overture_decoder/hint_solution.data \
    --rename-input Instruction --outdir build/decoder-level
powershell -File tests/kind-oracle-playtest.ps1 -Kind 0x2c \
    -CircuitPath build/decoder-level/circuit.data -Run 5
```

结果（`build/kind-oracle-2c/board-dump.txt`）：游戏**加载并运行了我们部署的电路**——
`components=10 wires=14`，里面正是那份解的七个元件（`0x11 Splitter`、`0x2c` 译码器、
`0x6f Maker`、`0x6a Input`、四个 `0x3a Output`）**加上探针放上去的第九个元件**。每条导线
都带一个状态偏移与**它当时的字节值**，而且线到线之间共享偏移（`268` 被三根导线共用、
`266` 两根、`270` 两根、`264` 两根），偏移以 2 字节为步长分配（256/258/260/…/270）。

**与 simcore 的比对**（这一条是可以直接对上的）：

| 量到的 | simcore 的预测 |
|---|---|
| `wire index=2 id=2 from=-2,-2 to=-2,-13 width=1 slot=264 value=1` —— 这根线两端正好是译码器的 `out0 (1,-1)` 与第一个 Output `0x3a (-5,-1)` | 关卡 Input 的初值是 0，地址 0 应当只点亮 `out0` |

也就是说：**游戏自己的仿真里，地址 0 时 `out0` 是 1**，与 `deviceDecoder(2,false)` 的
"输出 0 为 1、其余为 0"一致。这是一份**单点读数**，不是完整真值表——它确认了方向、确认了
"一位热码的第 0 位"这半边，还没有量地址 1/2/3（那需要驱动输入变化，或跑四次改初值）。

**下一轮**：把这份读数扩成真值表——改关卡 Input 的初值跑四次（或找一条能让值变化的驱动
路径），逐条与 simcore 的四个输出比对；然后再把同一套装置用到 `0x27` Register（它的入口
不在棋盘上，量法要单独定）与 `0x37` Delay Line（引脚几何已经在真机上出现：`wire record`
里能看到它的端点）上。

### 5.6 扫描真的跑起来了，但译码器没跟着输入变（本轮结论）

修掉两个自己的错之后，四次扫描跑出来了：

1. **扫描必须先有"跑仿真"那一段**：第一版把扫描分支挂在 run 阶段之后，而那次调用没给
   `-Run`，于是整个状态机从没进过扫描，直接 dump 了棋盘。现在扫描隐含 `run=1`，并把
   实际读到的配置打进日志。
2. **驱动要写的是棋盘上的元件下标**：关卡自己的 Input 是 **index 4**（不是文件名里的 8）。
   写对之后能确认驱动生效：8 位那根导线（`w11@256`）在四次里读出 **0,1,2,3**。

四次读数（`build/kind-oracle-2c/board-dump.txt`）：

```text
in=0  w2@264=1  w4@262=0  w13@264=1  w11@256=0
in=1  w2@264=1  w4@262=0  w13@264=1  w11@256=1
in=2  w2@264=1  w4@262=0  w13@264=1  w11@256=2
in=3  w2@264=1  w4@262=0  w13@264=1  w11@256=3
```

**关卡输入的值确实进了棋盘**（`w11` 跟着走），但**译码器的输入线 `w4@262` 始终是 0、
输出线 `w2@264` 始终是 1**——也就是说这份电路里 Splitter→译码器那一段没有跟着输入动。
两种解释，本轮**没有定论**：

- 游戏没有把"关卡 Input 的初值"传播进它下游的线（关卡输入靠 `input_replay` 每周期刷新，
  而这块板子在沙盒里、没有关卡的 `test.si` 在驱动它）；
- 或者我对"哪根线是译码器输入"的判定有误（那根 2 位线只读出低字节，且它在真机上是否
  与 `w11` 同网还没独立验证）。

**所以：装置可用了（逐值扫描、多字节读取都在真机上验证过），但译码器的真值表还没拿到。**
下一轮的两个候选做法：①把 2 位线也按 2 字节读（已经改了）并确认它与 `w11` 是否同网；
②不用关卡 Input，改用探针在棋盘上**放两个常量元件**当驱动源（`PLACE_COMPONENT` 已经验证
可用），这样就绕开 `input_replay` 这条不确定的路。

### 5.7 换驱动方式再测一次：结论是"这块板子在沙盒里没被重算"（本轮）

第 ② 条做了：探针新增 `TC_KIND_ORACLE_EXTRA`（`kind@x,y;kind@x,y`，在放目标之前先放别的
元件）。真机确认**两个 ON 常量（`0x02`）能放到棋盘上**：

```text
component index=1 kind=0x02 x=4 y=0 rotation=0 id=3067501558058112619
component index=2 kind=0x02 x=4 y=2 rotation=0 id=3431604179044659400
component index=3 kind=0x2c x=0 y=0 rotation=0 id=3511385219071665648
```

但**读数形状与上一轮完全一样**：绝大多数导线的字节是 0，下游没有跟着常量动。两次不同的
驱动方式（关卡 Input 的 `write_input`、棋盘上的常量元件）给出同一种形状，而 `sim.run` 与
`write_input` 都返回成功——所以这不是驱动方式的问题，而是：

> **沙盒里那块板子，在关卡没有自己的 `test.si`、也没有声明 I/O 时，游戏不会重算它的线。**

要让游戏真的算一遍，得让关卡**有 I/O 与测试**。下一轮的具体做法：把
`campaign/overture_decoder` 的 `meta.txt`/`test.si`（以及 `ui.txt`、`label.txt`）连同我们
的 `circuit.data` 一起部署成那个关卡，让游戏按关卡自己的测试去驱动输入、我们只负责读线；
或者改用一个**已经有测试**的关卡（例如 `byte_mux`）当宿主，把被测 kind 画进它的板子。

### 5.8 完整关卡契约也部署了，结论不变（本轮，装置到此收手）

第 5.7 节那条也做了：试玩脚本新增 `-LevelSource`，把 `campaign/overture_decoder` 的
`meta.txt`/`test.si`/`ui.txt`/`label.txt` 连同我们的 `circuit.data` 一起部署进沙盒关卡
（已经确认写出 `Copied overture_decoder's meta/test/ui/label into campaign\sandbox`）。
读数**与之前逐字相同**：`w4@262=0`、`w2@264=1` 恒定，其余字节基本为 0。

到此为止，**三种做法都试过了**：

| 做法 | 结果 |
|---|---|
| 只部署 `circuit.data` | 值不传播 |
| 部署电路 + 用常量元件（`0x02`）驱动 | 形状相同 |
| 部署电路 + 完整关卡契约（meta/test/ui/label） | **形状仍相同** |
| 再加长等待（run 60→300 帧、每值 30→120 帧） | 仍相同 |

**因此结论收紧为**：这块板子的线状态字节**从头到尾没有被重算过**——它们是关卡加载时的
初值，而不是仿真的结果。三种驱动、两种等待时长给出同一种形状，`sim.run` 与 `write_input`
都返回成功，所以问题不在驱动、不在关卡契约、也不在时间。**要让它算一遍，需要在放置元件
之后让游戏重新编译这块板子**（仓库里已有 `compile_request__modelZutilities_u…` 的用法，
见 `tests/component-placement-probe.cpp`），那是 `kind-oracle` 之外的一件独立工作。

**这条线在此收手**（第 17 轮决定）。装置本身是有价值的、已真机验证的部分：
`PLACE_COMPONENT` 能放内置 kind、多放常量也行、元件与导线记录能读回、`tc.io_value` 能
按棋盘下标驱动输入、导线槽位能按字节（含多字节）读出。缺的只是"让游戏重算"这一环。

### 5.9 缺的那一环：怎么接（第 18 轮查明，未实现）

**具体的接法已经有了**，不是未知问题：

```c
/* tests/component-capacity-probe.cpp 的两行就是全部 */
compile_request = resolve_symbol("preorder__modelZsimulationZcompile95thread_u3523");
compile_request(model, &progress, 0);      /* model 是 TCGameModel 的模型指针 */
```

`preorder` 就是"让游戏重新编译这块板子"的入口（`component-capacity-probe` 在放完元件后
靠它把编译跑起来）。**缺的是 `model` 指针怎么拿**：`component-capacity-probe` 是从
`handle_update_wire` 的钩子里取的（`updateOriginal(m, …)` 的第一个参数），而
`kind-oracle` 目前一个钩子都没挂（它复用了 `enter-board` 的钩子位，见 §5.1 第 1 条）。
两条可行路线：

1. 在探针里再挂一个 `handle_update_wire`（与 `enter-board` 的 `igInvisibleButton` 不冲突），
   从它的第一个参数取 `model`；
2. 或者看 `tc.board` / `tc.simulation` 的服务表里有没有直接把模型指针交出来的入口
   （`TC_SIM_STATE_DEEP` 那套探针是从 `get_sim_state(model, …)` 拿的，也要模型）。

接上之后 `kind-oracle` 的流程就是：放元件（含常量）→ `preorder` 编译 → 等若干帧 →
`write_input` 或改写常量 → 再编译 → 读导线字节。**这一步落地之前，行为真值表拿不到**；
本轮没有实现它，也没有把 §5.8 的结论改口。

### 5.10 真去接了：三条都挂上了，行为读数还是没出来（第 19 轮）

第 5.9 节的两条路线都实现了，而且**真机确认挂上了**：

```text
kind-oracle: model capture armed                      <- handle_update_wire 钩子
Hook chain sim.do joined (priority 0)                 <- sim.do 链
kind-oracle: joined the sim.do chain for the model pointer
```

`preorder__modelZsimulationZcompile95thread_u3523` 也解析到了。但**模型指针要等游戏处理过一次
运行请求才出现**（`sim_do` 只在有 run 请求时触发），于是 `recompile()` 在第一次扫描之前一直
拿不到模型；改成"每帧重试 + 每个值重编一次"之后，读数反而变了形：`w11@256` 不再跟着输入走
（四行都是 0），而 `w2@264` 仍是 1。也就是说这一轮**既没有得到真值表，也没有保住上一轮那个
能用的形状**——这是一个失败实验，不是进展。

**下一步（留给下一轮，不再靠猜）**：先只做一件事——把 `preorder` 调用的**返回值/副作用**
记录下来（游戏侧是否有编译错误、编译是否真的发生过），再决定是"编译时机不对"还是
"`preorder` 不是这条路"。在弄清之前不动 `kind-oracle` 的主流程。

### 5.11 第 19 轮之后的收尾（第 20 轮）

- **回退**：把"每个值重编一次"去掉，读数**恢复到 §5.6 那个形状**（`w11` 又跟着输入走：
  0,1），也就是说那次变形确实是多余重编造成的，不是别的原因。
- **为什么 `recompile()` 根本没被调用**：两个钩子都挂上、`preorder` 也解析到了，但日志里
  **一次 `preorder progress` 都没有**。代码路径核对过：钩子安装与 `preorder_fn` 赋值都在
  `enter_board_load` 返回之后（`h->create_hook` 没有被清空，"model capture armed" 也打出来了），
  所以最可能的是 **`sim_model` 始终是 null**——即 `tc::hook::simDoArgs(call)->model` 没给出
  模型指针。下一轮第一件事就一行：在 `interceptedSimDo` 里把 `args->model` 的**原始值**打进
  日志，确认这一点，而不是继续推断。
- 为了让下一次能一眼看出编译有没有发生，`recompile()` 现在会把 `preorder` 交回的 progress
  字符串（长度 + 内容）写进日志。

### 5.12 第 21 轮：模型指针是好的，编译那一步没被执行

上一条假设**被真机推翻**——`sim.do` 链节确实带着有效模型指针：

```text
kind-oracle: sim.do link fired, args=000000c525990f90 model=000000c5259e2770
（连续多次，模型指针稳定）
```

所以 `sim_model != nullptr`，而日志里**依然没有 `preorder`/`recompile` 的任何输出**，说明
`recompile()` 的调用点根本没走到。核对调用点是
`if (extra_index >= extra_placements.size() && placed_frame >= 0) recompile();`，剩下的唯一
解释是 **`placed_frame` 仍是 -1**，也就是"放置命令以 SUCCEEDED 结束"这个条件从未成立
（`placed_frame` 只在 `status.state == SUCCEEDED` 那一支里被置 0）。

**这就把问题从"拿不到模型"改成了一句可验证的话**：放置命令的状态到底停在哪一个 state。
下一轮只需把 `TCCommandStatusV1` 的 `state/result/completed_frame` 每次都打进日志（一行），
即可定位是"命令没成功"还是"状态读取的分支写错了"。

**留给结论的余地**：到这一轮为止，`kind-oracle` 已真机确认的部分是——能进关卡、能放内置
kind（含多个）、能读回元件与导线记录、能按棋盘下标驱动输入、能读多字节槽位、`sim.do` 链节
能拿到模型指针。**没能确认的是"程序化放置之后让游戏重算这块板子"**，而行为真值表依赖它。

### 5.14 第 22 轮：找到了一个真 bug，但状态行仍然没打出来

**找到并修掉一个真 bug**：放置命令的状态检查原先**写在 `kWaitingForPlacement` 分支里面**，
而状态机在发出 run 请求后就转到了 `kRunning`——那个分支再也不会执行，所以 `placed_frame`
永远是 -1，`recompile()` 的条件永远不成立。这解释了 §5.12 的"模型是好的、编译却没跑"。

**修法**：把状态检查提到 stage 门之前（每帧读一次，`request != 0` 时），并在成功时把
`placed_frame` 置 0 再继续。代码已编译通过。

**但真机跑完，`placement state=…` 这一行仍然没有出现**，读数也与之前逐字相同。也就是说
`placed_frame < 0 && request != 0` 这个条件本身没成立：要么帧回调里看到的 `request` 是 0
（而放置提交那一帧日志明确写着 `request=1`），要么状态检查那一段根本没被执行到。**这是下一轮
唯一要看的地方**：在帧回调最开头无条件打一行 `request`/`stage`/`placed_frame`，把这三者的
真实值固定下来，再谈别的。

**到这一轮为止，行为真值表仍然只有一个单点读数**（第 13 轮：输入 0 → 译码器 `out0` 那条线
读出 1）。下文 §5.13 的能力清单与 19 个未支持 kind 的处置不变。

### 5.15 第 23 轮：状态机修好了，`preorder` 却一次都没被调用

那三值日志终于把真相打出来了：

```text
trace stage=1 request=1 placed_frame=-1 extra=0/0
trace stage=1 request=1 placed_frame=1  extra=0/0
trace stage=1 request=1 placed_frame=2  extra=0/0
trace stage=2 request=1 placed_frame=6  extra=0/0
```

也就是说**第 22 轮那个 bug 修对了**：放置命令以 SUCCEEDED 结束、`placed_frame` 开始累加、
状态机从 1 走到 2。命令总线那一环是好用的。

**但 `recompile()` 依然没生效**：日志里 `preorder` 出现 **0 次**。而它的前置条件是
`!preorder_fn || !sim_model`——`sim_model` 已被证明是好的（`sim.do link fired … model=…24e0`），
所以**只剩一种解释：`preorder_fn` 解析出来是 null**，函数在第一行就返回了。

**下一轮唯一要看的**：在 `tc_mod_load` 里把
`preorder__modelZsimulationZcompile95thread_u3523` 的解析结果直接打进日志（一行）。若为 null，
就在符号画像里换一个名字（`compile_request`、`preorder` 的其他 mangle 形式，或
`component-capacity-probe.cpp` 用的那个符号是否随构建变了）；若不为 null，那就是调用时机
（`sim_model` 在 `recompile()` 那一刻还是 null，因为我们是在同一帧里先 recompile 再 run）。

### 5.16 第 24 轮：符号是好的，编译调用仍然没有落地 —— 这条线到此阻塞

那一行日志跑掉了，**符号解析完全正常**：

```text
kind-oracle: preorder symbol=00007ff6a2c31a20 fn=00007ff6a2c31a20 resolve=00007ffe7d9a2080
```

于是 `!preorder_fn` 不成立，唯一剩下的原因是 **`sim_model` 在 `recompile()` 被调用的那一刻
还是 null**——模型只有等 `sim.do` 响应过一次运行请求才出现，而我把编译请求放在了同一条路径的
前面。据此把条件改成"模型到手之后再请编译"（`if (sim_model && placed_frame >= 0) recompile();`）
并重跑，**`asked the game`/`preorder progress` 两行依然一次都没出现**，读数也逐字未变。

到这里，这条线**已确认的事实**与**没能确认的一件事**可以定下来了：

| 已确认（真机证据） | 没能确认 |
|---|---|
| 符号 `preorder__…u3523` 解析成功（指针非空） | 让游戏在**程序化放置之后**重算棋盘 |
| `sim.do` 链节带来有效模型指针（稳定非空） | ↑ 因此行为真值表拿不到 |
| 放置命令以 SUCCEEDED 结束、状态机 0→1→2 正常 | |
| 能放内置 kind（含多个）、读回元件/导线记录、按棋盘下标驱动输入、读多字节槽位 | |

**因此这条线判为阻塞**（连续多轮同一条件：探针无法让游戏重算放置后的棋盘，且所有已知前置
条件都已用真机日志逐一排除）。行为真值表的全部证据仍是第 13 轮那一个单点读数。

**给接手人的最短路径**：不要再从 `kind-oracle` 的帧回调里找原因——已经证明符号、模型、命令、
状态机四者都正常。最可能的差异在 `component-capacity-probe.cpp` 的调用**上下文**：它在
`set_sim_test(model, 0, 1)` 之后才 `compile_request(model, &progress, 0)`，也就是说游戏可能
要求"先进入测试态"才肯编译。下一次实验就照它抄：先 `set_sim_test`，再 `preorder`。

### 5.17 第 25 轮：`set_sim_test` 也接上了，`want-compile` 那行仍然没出现

按上一条抄了：`set_sim_test__modelZutilities_u6840` 解析成功（`set_sim_test=…cea0`），并在
`recompile()` 里先调它、再调 `preorder`。为了定位，又在"该编译了"这个决策点加了一行日志
（打印 `sim_model` 与 `placed_frame`）。**结果是这一行也没出现**——也就是说**根因不在
`preorder`，而在决策点之前的条件**：`placed_frame >= 0` 在帧回调里（我放这段的位置）
没有成立，或者那段代码根本没被执行到。**下一步要看的就是这一行日志为什么没有输出**，
而不是再看 `preorder`。

到这里，"让游戏重算棋盘"这件事的诊断已经把范围压到**一个日志语句没被执行**这一点上。
本轮到此为止：不再改这个探针，把结论与下一步写在这里。

### 5.18 第 26 轮：帧状态机重写，之前三轮的结论被推翻（这次是好事）

那个帧回调被我从零重写成**扁平流水线**：一步一个函数、返回下一步、**每次转换都打日志**
（`step A -> B (after N frame(s))`），不再有嵌套分支。重写后第一次跑就把真相全打出来了：

```text
step wait-for-board -> place-target (after 340 frame(s))
step place-target -> wait-for-placement (after 1 frame(s))
placement state=3 result=0 frame=341
step wait-for-placement -> want-compile (after 1 frame(s))
want-compile model=0000007c773e2650 preorder=00007ff6d6761a20 after 10 frames
preorder progress empty
asked the game to recompile the board
step want-compile -> run-once (after 10 frame(s))
step run-once -> sweep-write (after 1 frame(s))
step sweep-write -> sweep-wait (after 1 frame(s))
step sweep-wait -> sweep-write (after 120 frame(s))
```

**所以第 23–25 轮的三条结论都要更正**：

| 之前的结论 | 真相 |
|---|---|
| "`preorder` 一次都没被调用" | **被调用了**（`asked the game to recompile the board`），而且没有报错（`progress empty` 不是错误） |
| "`preorder_fn` 是 null" | 不是，指针有效 |
| "`sim_model` 那一刻还是 null" | 不是，`model=…2650` 非空 |

**真正没解决的仍然是同一件事**：编译请求发出去了、没有报错，但**扫描里 `w11@256` 四行都是 0**
（之前是能跟着输入走的 0,1,2,3）。所以要查的下一处是**这次重编是不是把棋盘的状态清掉了**
（重编后 `placed_frame`/槽位可能变了），或者 `sim.run` 到底有没有真的跑起来——
`sim.do` 链节的日志能直接回答后者。

**方法论上的一条教训**（写下来）：我连续三轮在**读日志的缺失**上做推断，而真正的错误是
**日志本身没被执行**。扁平化 + 每步转换都记一行，一次性解决了这个循环。以后再遇到"某件事
没发生"，第一步应该是确认"那行日志有没有执行"，而不是解释它为什么没输出。

### 5.19 第 27 轮：想直接看槽位表，撞上了一条 loader 契约

为了验证"重编把槽位重排了"这个假设，加了 `slotMap()`（把导线表打成 `id@slot` 串）并在
编译请求前后各打一次。结果第一次调用就返回 `unavailable`——因为**这个 helper 只给了
`wires` 缓冲**，而 loader 的 `capture_objects` 要求 `buffers->components` 也必须非空
（`native.hpp` 第 508 行：`component_capacity && !components` 直接判 `ARGUMENT`）。这是我自己
的实现疏漏，不是游戏的问题。

**所以这一轮的结论只有一条**：`slots before` 这个探针没拿到数据，槽位重排的假设**尚未验证**。
修法很小（给 helper 也分配 components 缓冲，照 `wireValues()` 的写法），下一轮一行就能拿到。

**到目前为止这条线的净结论没有变化**：编译被调用了、没有报错，但扫描里导线字节不随输入变
（§5.18）。行为真值表仍然只有第 13 轮那一个单点读数。

### 5.20 第 28 轮：槽位重排的假设被直接否掉

修好缓冲契约之后，编译请求前后的导线表**逐字相同**：

```text
slots before = wires=14 w0@268 w1@270 w2@264 w3@260 w4@262 w5@266 w6@268 w7@266 w8@266 w9@258 w10@270 w11@256 w12@268 w13@264
slots after  = wires=14 w0@268 w1@270 w2@264 w3@260 w4@262 w5@266 w6@268 w7@266 w8@266 w9@258 w10@270 w11@256 w12@268 w13@264
```

也就是说 **重编没有重排槽位**，"重编之后读错了地方"这个解释**不成立**。结合 §5.6–§5.18 的全部
读数，可以确定的是：

- 驱动到位（`w11@256` 曾经跟着 `write_input` 走 0,1,2,3）；
- 槽位稳定（本轮直接比对）；
- 编译被调用且无报错（§5.18）；
- 命令、状态机、模型指针、符号全部正常。

**唯一剩下的解释是：游戏没有对这块板子执行仿真**——`sim.run` 提交成功、`sim.do` 链节也带着
有效模型与目标周期回调了，但没有任何字节被写回状态数组。这需要从**仿真线程/程序生成**那一侧
看，不是 `kind-oracle` 这个帧回调能解决的。

**这条线在此判为阻塞**（同一条具体条件已持续多轮，且所有可从探针侧检查的前置条件都已用真机
日志逐一排除或直接比对）。行为真值表的全部证据仍是第 13 轮那一个单点读数（输入 0 → `out0=1`）。

### 5.13 这条线的状态（交接用）

**已完成、可复用**（都不依赖那个没通的环节）：

| 能力 | 证据 |
|---|---|
| 隔离沙盒 + 部署 + 断言 + 留档的整套流程 | `tests/kind-oracle-playtest.ps1` 多次跑通，证据在 `build/kind-oracle-*/` |
| 用命令总线放**任意内置 kind** | 五份读数（`0x2c`/`0x27`/`0x6a`/`0x3e`，以及带坐标旋转的一次） |
| 一次放多个元件（常量也能放） | 两个 `0x02` 常量的读数 |
| 读回元件记录、导线记录（含 `state_byte_offset`） | 每份 dump 的 `component`/`wire` 行 |
| 按棋盘下标驱动关卡输入 | `w11@256` 跟着 `write_input` 走 0,1,2,3 |
| 多字节槽位读取 | 2 位/8 位线的读取修正 |
| `sim.do` 链节拿模型指针 | `sim.do link fired … model=…2770` |

**未通的那一环**：程序化放置之后让游戏重算棋盘。下一步唯一该做的实验是**把放置命令的状态
（`state`/`result`/`completed_frame`）逐帧打出来**——一行日志就能把范围缩到"命令没成功"
或"状态分支写错"。

**行为真值表当前的全部证据**：第 13 轮那一个单点（关卡输入初值 0 时译码器 `out0` 那条线
读出 1）。这份证据支持"一位热码的第 0 位"，但**不支持**把它写成"译码器语义已验"。

**因此 19 个未支持 kind 的最终处置**（第 21 轮决定，交给后续）：

- 其中 **12 个在游戏自己的 217 份电路里一次都没出现**（Auto Register、Auto Delay Line、
  7 Segment、Static Value、Static Evaluator、Auto Static Indexer、Halt、Disassembling probe、
  Level Component、Level And Delay Component、两个无名槽）。它们**不阻塞任何真实板子**，
  按"识别并明确报未支持"交付即可。
- 剩下 **7 个**（`0x27` Register、`0x26` Counter、`0x6a` Input、`0x3e` Switched Input、
  `0x37` Delay Line、`0x53` Memory Probe、`0x2f` Splitter）的语义都**不由棋盘上的线决定**，
  需要真机行为读数或反汇编；在拿到读数之前它们继续保持"未支持 + 已写明原因"，
  **不写近似实现**（红线 6）。

### 5.5 `tc.io_value`：扫描服务的接线（历史记录，结论见 §5.6）

`tc.io_value` 服务提供 `write_input`/`read_input`/`flip_input`（按**棋盘上的元件下标**读写
全局输入），所以扫描不必改关卡文件。为此加了：

- 探针：`TC_KIND_ORACLE_SWEEP_INPUT/FROM/TO`，进入扫描后逐个值 `write_input` → 让游戏跑
  一会儿 → 把每条导线的 `id@slot=value` 记成一行；`wireValues()` 负责取行。
- 试玩脚本：`-SweepInput/-SweepFrom/-SweepTo` 三个参数，已通过 PowerShell 解析检查。

**但这一轮实际跑出来的是普通 dump，不是扫描**：loader 日志里既有 `sim.run` 也有
`sweep input` 记录都**没有出现**，说明探针根本没进那两个分支——最可能是 `sweep_input`
仍是 -1（环境变量没被探针看到），也可能是 `sim.run` 那一段的时序问题。**下一轮第一件事**
就是先打印探针实际读到的三个环境变量值，确认参数通路，再把四次扫描跑出来。装置本身
（`write_input` 通路、逐值取行）都还没被真机验证过，本轮不声称它可用。

**还差的一步**：见上面第 1–4 条——四道坎都跨过去了，**装置可用**。剩下的是行为测量：

**还差的一步（写清楚，免得下一轮又从头摸）**：见上表后面的第 1–3 条——钩子位已经理清、
进棋盘也已经通了，**卡在 `captureBoardObjects` 返回 -2**。这一处修好之后，下面那句话才成立：

1. 用命令总线在空沙盒板上**放一个内置 kind**（`validateCommand` 明确允许内置 kind，
   `board_edits::placementKind` 已经这么用）；
2. 用公开的 Board V4/V5 读回该实例的引脚表与记录字段（含 `+0x70` 那类未知字段的原始值）；
3. 用 `tc.simulation` 跑够 N 个周期，**逐周期读状态数组**——导线与引脚的当前值就在
   `wire.slot` 指向的字节里，这一步是只读的，`sim-state-probe` 已经证明可行；
4. 对每个输入钉死值（常量件是现成的驱动），把输出脚的值录成一行语料；
5. 把语料写成 `simcore/tests/data/kind-oracle-*.txt`，离线单测直接读它比对 simcore 的器件。

**验收**：每个 kind 至少一条"输入→输出"语料，且 `simcore` 的器件与语料**逐位相同**；
量到的位序/字段写进本文 §4 的对应行，并把该 kind 从"未支持"移进器件库。
**顺序按 §4 表的出现次数**：`0x27` Register（71）→ `0x6a` Input（39）→ `0x3e`
Switched Input（20）→ `0x37` Delay Line（11）→ `0x53` Memory Probe（10）→ `0x26`
Counter（3）→ `0x4d`（1）；**出现 0 次的那十二个排在最后**，其中几个（Halt、7 Segment、
Static Value）本来就不是棋盘器件，可能永远只报"未支持"。
另有一件要顺带量的：`0x2d` 的 `in1` 到底是不是使能、极性如何（§3.5）。

**成本与风险**：探针是新增一个 `tests/*-probe.cpp` + 一个 `tools/test.ps1` 用例；
风险是"合成输入驱动不了棋盘"（`component-hitbox-path.md` 记过这个），所以第 3 步先只读，
不依赖合成鼠标。

## 6. 相关文档

| 文档 | 内容 |
|---|---|
| [PLAN-sandbox-simulator.md](../PLAN-sandbox-simulator.md) | 沙盒内核的总计划（S1–S5）、红线 6 |
| [component-appearance.md](component-appearance.md) | 原版元件外观与精灵；引脚点/标签是另一层 |
| [board-object-fields.md](board-object-fields.md) | 元件记录字段的实测表（§7 内存端口） |
| `simcore/README.md` | 器件库当前的公开边界 |
| `tools/netlist-view.py` | 把游戏自己的解读成网表（本轮新增的离线尺子） |

## 7. 第七刀：存储家族、关卡件与剩下那一个（2026-09-30）

§4 的表把 19 个 kind 分成三组，说它们"缺一次测量"。这一轮把那三组的测量都做了——
而且用的不是黑盒扫输入输出，是**游戏自己编译出来的程序**：给一块带该 kind 的板子，
让游戏编译它，读 `native-logic-source-*.txt`（`TC_MODLOADER_DUMP_SOURCE=1`），生成的代码
就是契约。工具是新的 `tests/kind-source-dump.ps1`（跑一块板子，把生成的程序收进
`build/kind-source-<tag>/`）。

### 7.1 引脚：原型表的**第二个**列表（`+0x70`/`+0x78`）

存储件的输入脚不在原型表的声明列表里，但它们在 `+0x70`（数量）/`+0x78`（指针）指向的
**第二个 pin 列表**里，条目布局与声明列表一样（0x38 字节）。`tests/kind-list-probe.cpp`
现在把这张表也 dump 出来（`hidden0=(…)`），于是几何一次问清：

| kind | 名字 | 声明脚 | 隐藏脚 |
|---|---|---|---|
| `0x27` / `0x0e` | Register（字 / 位） | out0=(3,0) | **hidden0=(-3,-1) = enable（1 位）**，hidden1=(-3,0) = data |
| `0x26` | Counter | out0=(3,0) | hidden0=(-3,-1) = reset（1 位），hidden1=(-3,0) = data（宽度来源） |
| `0x37` / `0x0d` | Delay Line（字 / 位） | out0=(3,0) | hidden0=(-3,0) = data |
| `0x32` | Auto Register | in0=(0,-2) = 蓝脚（宽度） | hidden0=(-3,-1) = enable，hidden1=(-3,0) = data |
| `0x77` | Auto Delay Line | in0=(0,-2) = 蓝脚 | hidden0=(-3,0) = data |
| `0x35` | （无名） | — | 与 `0x36` Load port 声明脚**逐条相同**：它就是 Load port 的无名孪生 |

**记录算的是总数**：真机把这件事直接说了出来。第一版只按"声明脚"报数，沙盒立刻拒绝：

```text
sandbox-sim: cannot run this board: component 3 (Counter) has 2/1 pins in its record but the prototype has 0/1
```

所以 `KindPins` 现在同时带 `input_count`（**总数**，与记录对齐）与 `declared_inputs`
（其中多少来自声明列表），隐藏脚排在声明脚之后——这与游戏自己的编号一致。

### 7.2 行为：游戏自己写的程序

```text
// com_register_word 13 … Reg 0
let value_3 = load(<U13>, #SIMULATION_STATE + 332)
store(#SIMULATION_STATE + 333, value_3)                  // 输出脚
// LATE
if (U1 vid272) == 1 { store(#SIMULATION_STATE + 332, U13 (vid317)) }

// com_delay_line_word LATE
store(#SIMULATION_STATE + N, U8 (vid265))

// com_counter LATE
if (U1 0x0) == 1 { store(state + N, 0) }
else { store(state + N, (load(state + N) & <宽度掩码> + 1)) }
```

三件行为因此都不是猜的：**寄存器在 enable 为 1 时锁存数据、否则保持**；**延迟线每个周期
锁存输入**；**计数器每个周期加一、reset 为 1 时归零**。三者的锁存都发生在游戏的 **LATE**
阶段——也就是 `cycle += 1` 之前那一步。

### 7.3 落到 simcore 的三处改动

| 改动 | 位置 | 说明 |
|---|---|---|
| 状态器件 | `devices.hpp` 的 `deviceRegister` / `deviceDelayLine` / `deviceCounter` | 器件第一次有了**自己的存储**（`DeviceSpec::state_behavior` + `state_size`），引擎只在锁存那一次提交状态；`output_is_state` 让"当前存储值"从第一次求值起就驱动输出（否则一个从未使能的寄存器会让输出停在 Z） |
| 每周期时钟 | `board.hpp` 的 `BuildResult::cycle_token` + `runtime.hpp` | 板级一条私有网，运行时在每个周期边界翻转（与 RAM 端口链的 token 同一个把戏）：存储件的时钟就是它，于是"每周期锁存一次"不需要引擎知道周期是什么 |
| 关卡件与其余 | `board.hpp` 的 `storageKindSpec` / `kindIsSource` / `kindIsSink` | `0x6a` Input、`0x3e` Switched Input 是**源**（值来自关卡/游戏状态数组）；`0x53`/`0x33`/`0x59`/`0x57`/`0x4d`/`0x75` 是**观察者**（游戏对它们一行代码都不生成）；`0x29`/`0x3b` 是关卡地图件（游戏生成的是 `level 值 & 引脚`，沙盒没有关卡值，因此输出恒 0、输入进观察集）；`0x32`/`0x5a`/`0x5e` 是常量源；`0x2f` 是"把 2n 位劈成两个 n 位"的字宽 Splitter（`deviceWordSplit`，n 由导线解出）；`0x35` 按 Load port 处理 |

### 7.4 门槛

- `simcore/tests/simcore-tests.cpp` 新增 `testStorageFamily()`（含"寄存器不跟随数据脚"、
  "enable 低时保持"、"延迟线晚一拍"、"计数器每周期加一 + reset 归零"）与
  `testWordSplitter()`；`simcore.ps1` **357/357**、`view-tests.ps1` **188/188**。
  离线测试抓到的唯一一个真 bug 是**位序**：`BitVector` 是 MSB-first，而新写的加法从元素 0
  开始进位，读出来是 `10000000` 而不是 `00000001`——`toUint64` 的注释就是契约，照它写。
- 真机：`tests/sandbox-storage-playtest.ps1`（目录条目 `sandbox-storage`）用
  `tools/kind-fixture.py --standalone` 写一块带 `0x2e` 常量 + 寄存器 + 计数器 + 延迟线的板子，
  部署成沙盒关卡，断言加载器**不再拒绝**并且绑定的器件清单里三个都在：

  ```text
  sandbox-sim: bound board -> nets=2 devices=4 sources=0 observed=0 cycles=0
  sandbox-sim: devices -> u2_0_register, u3_2_counter, u4_1_delay, u1_constant
  ```

- 顺带写下的两条工具经验：`tools/kind-fixture.py` 写出来的记录里**两个元件不能共用同一个
  `identity`**（共用时游戏会静默丢掉其中一个，表现为"我明明写了寄存器，板上没有"）；
  关卡自己的 Input/Output 元件（`0x3d`/`0x45`）在沙盒里会被**抹成 kind 0**，除非关卡的
  `ui.txt` 声明了它们——所以夹具的 `--standalone` 模式用 Constant 驱动、输出不接。

### 7.5 这一刀之后仍然"没量到"的（写清楚，不假装）

| 项 | 现状 |
|---|---|
| `0x5a` Static Value 的**值字段** | 游戏对一块默认记录生成 `U64 8`，而这个 8 恰好等于记录的宽度；把夹具的宽度改成 13 后该元件被游戏丢弃，所以"值到底存在哪个字段"仍未钉死。simcore 目前按"宽度字段即值"驱动 |
| `0x26` Counter 的第二个隐藏脚（data） | 游戏的程序只读 reset 一个条件；那个自动宽度的 data 脚看起来是**宽度来源**（与 Auto 版的蓝脚同族），不参与运算 |
| `0x29`/`0x3b` 关卡件的 level 值 | 沙盒里没有关卡值（沙盒关卡的测试是空的），因此输出恒 0；真要让它们跟随关卡，得先有"关卡值"这一层 |
| `0x57` Halt | 游戏会 `halt()` 停下仿真；沙盒没有"停"这个概念，按观察者处理（棋盘值自然地不再变化）。这条差异写在 `kindIsSink` 的注释里 |
| `0x50` | 游戏自己拒绝放置，交付形态就是"识别 + 明确报未支持" |

## 8. 自定义元件（kind 0x4E）：另有一条路

内置元件盘点完之后剩下的就是自定义元件：它们不是原型表里的 kind，而是游戏从一份电路解析
出来的**定义**。沙盒现在把定义**展平**进网表（组件数组 `+0xe0`/`+0xe8`、导线数组
`+0x100`/`+0x108`，元素格式与棋盘记录相同；按序号桥接边界脚）。玩家那块带五种自制元件的
CPU 板因此从"整块拒跑"变成 nets=130 / devices=103 的正常网表。测量、展平算法与还没做的
部分单独记在 [custom-components.md](custom-components.md)。
