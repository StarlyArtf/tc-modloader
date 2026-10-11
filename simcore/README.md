# simcore —— 沙盒独立仿真内核

[docs/PLAN-sandbox-simulator.md](../docs/PLAN-sandbox-simulator.md) 的 S1 内核实现。
纯 C++17，**零游戏依赖**：可以在游戏外编译、运行、出波形，因此"我们的仿真器对不对"
可以交给外部裁判（Icarus Verilog）而不是自证。

```text
simcore/
  include/tcsim/
    value.hpp       L0 四态值与强度解析
    time.hpp        L1 整数刻度与周期换算
    delay.hpp       L3 时序弧、tplh/tphl、惯性/传输、时序库数据结构
    scheduler.hpp   L2 时间轮 + 溢出堆 + active/NBA/monitor 分层
    netlist.hpp     L4 网表、引脚、驱动/负载、扇出
    devices.hpp     L3 器件库（组合门、字宽运算、Mux、常量、Splitter/Maker、
                    字宽 Splitter、DFF，以及存储家族：寄存器/计数器/延迟线）
    memory.hpp      游戏兼容的稀疏 RAM、8/16/32 位 Load/Store 端口
    screen.hpp      屏幕（0x5b）的设置表与像素：手册里的四种模式、像素格式与分辨率
    board.hpp       S2：游戏板级记录 → 网表（引脚、导线、RAM 插座、写回槽；
                    存储家族的隐藏脚与每周期时钟网也在这里接线）
    engine.hpp      事件驱动求值、delta 环、槽位式输出更新、违例报告
    runtime.hpp     周期推进、宽总线输入/写回、RAM 初始化入口
    observe.hpp     L5 逐事件 trace、规范事件表、VCD 导出
    verilog.hpp     网表 → Verilog（交叉验证用）
  tests/
    circuits.hpp         单测与交叉验证共用的电路
    simcore-tests.cpp    357 项断言 + --emit-verilog / --trace
    simcore.ps1          编译并运行（离线门槛）
    vcd-compare.js       iverilog 的 VCD ↔ 我们的事件表
```

## 怎么跑

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File simcore/tests/simcore.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/simcore-iverilog.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File simcore/tests/semantics-probes.ps1
```

最后一个不是测试，是**语义测量**：`simcore/tests/probes/` 里三个 Verilog 探针把
"首次赋值走不走延迟""上升/下降延迟是不是两个数""多驱动怎么解析""环的周期是多少"
量出来，内核的语义就是照着它们写的。

两者都登记在 `tests/test-catalog.json` 里（`simcore`、`simcore-iverilog`，fast 层），
可以随套件一起跑：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast -Name 'simcore*'
```

外部裁判需要 Icarus Verilog；没装时 `tools/simcore-iverilog.ps1` 打印跳过原因并以 0 退出，
其余离线测试照常：

```powershell
pacman -S --needed mingw-w64-ucrt-x86_64-iverilog   # MSYS2 UCRT64
```

计划 §11 写的是 `ctest`；本仓库没有 CMake，所有离线测试都是"PowerShell 脚本 + g++ 编译"，
所以 simcore 用同一种形状（`simcore/tests/simcore.ps1`），没有引入第二套构建系统。

## 已经对过的东西

`tools/simcore-iverilog.ps1` 把下面八个电路导出成 Verilog（`assign #(tplh,tphl)`、
`always @(posedge clk) q <= #ctoq d`），用 iverilog 13.0 跑一遍，再和内核自己的逐事件表
按"每个时间点、每根网"逐一比对。当前结果：**8 个电路全部一致**。

| 电路 | 覆盖的语义 |
|---|---|
| `delay_chain` | 每时序弧的 tplh/tphl、首次赋值同样走延迟、**惯性**脉冲滤波（窄脉冲被吞） |
| `multi_driver` | 多驱动解析：同意→该值、0/1 同强度→X、弱驱动输给强驱动、驱动关断→Z |
| `gate_unknown` | Z 输入按未知处理、0 主导 AND、1 主导 OR、常量 tie-off |
| `ring` | 三反相环释放后的振荡，周期 = 6 × 每级延迟（无 delta 环就做不出来） |
| `delta_chain` | 64 级零延迟链在同一时刻按 delta 轮收敛（递归实现会爆栈） |
| `dff_shift` | NBA 层语义 + clock-to-q：同一时刻读不到正要驱动的值 |
| `bus_split` | 4 位 Maker/Splitter 的位序与向量网解析 |
| `mux_word` | 游戏的字宽 Mux：选通为 1 走 B、为 0 走 A、未知时保留两路一致的位 |

单测覆盖波形看不到的部分：强度解析表本身、调度顺序 `(time, layer, seq)`、
时间轮与溢出堆两条路径、过去时刻入队计数、setup/hold 违例、零延迟振荡报错而不是死循环、
自环反相器停在 X（不误报振荡）、未驱动网报告、VCD/Verilog 文本形状，
以及"游戏记录里的自动宽度"怎么进器件（下节）。

## 边界

- 器件库目前是 S1 规模加三批：`buf/not/and2/and3/nand2/nand3/or2/or3/nor2/nor3/xor2/xor3/xnor2/xnor3/dff`
  加 `const`、`splitter`/`maker`，游戏自己的字宽 Mux（`0x2a`，`mux<宽度>`，一位的
  别名是目录里的 `mux2`），**字宽运算族**（`devices.hpp` 的 `WordOp`）：
  `addW`（带进位入/出）、`equalW`、`lessU`/`lessS`、`mulW`/`divW`/`modW`、
  `shlW`/`shrW`/`rolW`/`rorW`/`asrW`、`negateW`/`incW`/`notW`/`clzW`/`ctzW`、
  字宽 `andW`/`orW`/`nandW`/`norW`/`xorW`/`xnorW`，2 入 `concat2`，
  **位序/拼接族的 14 个 kind**（Splitter / Maker / Concatenator，走既有的
  `splitter<宽度>` 与 `maker<宽度>`，宽度由原型表自己的引脚形状决定），
  **Static Indexer**（`indexer<index>_<width>`，取字段规则来自游戏自己的 ISA），以及
  **两个接口引脚**（`pin<宽度>`，Input/Output Pin 的直通，没有驱动时输出 Z）。
  这一族的宽度由调用方给（板级接入从游戏记录 `+0xe0` 或数据脚导线上取），
  移位量是参数脚、宽度取自到达它的那根线。**没有的器件返回空指针、需要在接入层被
  报成"未支持"，不允许近似**（计划红线 6）。
- **Splitter/Maker 的位序是"最上面的叶子是最高位"**：原型表只说叶子的偏移与宽度、
  没说哪端是高位。取这个读法是因为它让两者互为精确逆运算（`bus_split` 已与
  Icarus Verilog 逐网对过）并与 2 入 Concatenator 由声明宽度强制的读法一致——
  这是命名层面的选择，不是发明行为，且**仍列在待测清单里**，见
  [../docs/research/simcore-kind-coverage.md](../docs/research/simcore-kind-coverage.md) §3.2。
- **字宽运算的四态规则和门不一样**：游戏生成的程序是 64 位字上的普通 C，它自己的参考
  仿真器把每个值都解析成具体数，所以字宽运算里**任意一个未定操作数位会把整个结果变成
  X**（比较类给一个 X），而不是只污染那一列。位级门仍保留逐位真值——那是门电路物理上
  的行为。理由与实现见 `include/tcsim/devices.hpp` 该族开头的说明。
- **还没有的 22 个 kind**（哨兵宽度、状态件的字段语义、译码器、几个探针、`0x6a` 的宽度
  来源）与每个缺的那一个数，列在
  [../docs/research/simcore-kind-coverage.md](../docs/research/simcore-kind-coverage.md)；
  现状可用 `python tools/kind-coverage.py` 重新点数，游戏自己怎么接这些元件可以用
  `python tools/netlist-view.py <关卡电路>` 直接看。- 字宽 Mux 的引脚顺序与宽度都取自游戏自己的原型表（`0x2a name="Mux" in0=(-1,-1,w1)
  in1=(-1,0,wauto) in2=(-1,1,wauto) out0=(2,0,wauto)`，`build/kinds.txt`）：**选通脚恒为一位**，
  两条数据脚取记录 `+0xe0` 里游戏算好的宽度（玩家真机记录上是 32），记录里没有才退回
  "数据引脚上最宽的那根导线"。真相见 `simcore/view/data/mux.txt`（真机记录的固化）与
  计划 §16。
- 多位网的上升/下降**成对延迟**还没对上 iverilog：它按新值的最低位选边，本内核按最高位，
  含 x/z 的值两家都没有可归纳的规则（量到的结果在 `simcore/tests/probes/probe4-vector-delays.v`）。
  所以总线器件一律用**同一个数**做弧延迟交叉验证（沙盒当前的时序库本来就是"每器件一个常数"），
  这条差异记在计划 §13.3，等游戏自己的时序表来说该选哪边。
- 时序库（S5）只有数据结构与查表入口，还没有文件格式；未命中时退化成"每器件一个常数"。
- **屏幕（`0x5b`）、Time（`0x5c`）、Keyboard（`0x5d`）已"认识"**：几何取自游戏自己的原型表，
  屏幕的三个脚（Enable 1 位 / Setting index 8 位 / Value 64 位）会锁存成一张设置表，
  `screen.hpp` 按游戏手册把设置翻译成像素（Pixel 8 / Pixel 32 可实现，ASCII 两种模式需要游戏的
  字库，明确报"未实现"）。Time/Keyboard 是**宿主源**：它们的输出值只有游戏知道，simcore 只把
  那些网标成外部驱动，不自己发明时间和按键。
  还差的一条数据桥：屏幕显示的是**它被链接的那颗 RAM**（游戏里屏幕角上的小红按钮选的），
  链接落在记录的哪个字段、以及那颗 RAM 的字节，都还没从真机量到——所以现在能在 simcore 里
  "认出并建模"屏幕，但还画不出它的画面。计划 §17 写了这条桥的两条走法。
- S2 已能读板级记录、按槽位收发宽总线，并按 RAM 插座中从上到下的顺序执行 Load/Store。
  RAM 内容通过 `BuildOptions::initialize_memory` 注入；游戏 `memory_manager` 的关卡重置数据还没有
  接到这个回调，因此未注入时按全零内存启动。
- VCD 的时间刻度是 `1ps`，一个 tick = 1ps，和 iverilog 导出的时间轴同一套坐标。

## 自定义元件

自定义元件（kind `0x4E`）在游戏里不是器件，而是一份**定义**：游戏把一份电路解析后放进它的
自定义原型表。沙盒在成表前把定义**展平**——读原型里的组件数组（`+0xe0`/`+0xe8`）与导线
数组（`+0x100`/`+0x108`，元素格式与棋盘记录相同），把内部件与内部线搬到远端坐标，再按序号
把定义的边界脚接到实例引脚上；内部网用 `kNoStateSlot` 明确"没有游戏的字节可写回"。
展平由沙盒插件做（`examples/sandbox-sim/plugin.cpp`），simcore 侧只多了"每侧 64 脚的几何表"
与那个哨兵。测量与边界见 docs/research/custom-components.md。
