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
    devices.hpp     L3 器件库（组合门、Mux、常量、Splitter/Maker、DFF）
    board.hpp       S2 半边：游戏的板级记录 → 网表（引脚几何、导线成网、写回槽）
    engine.hpp      事件驱动求值、delta 环、槽位式输出更新、违例报告
    observe.hpp     L5 逐事件 trace、规范事件表、VCD 导出
    verilog.hpp     网表 → Verilog（交叉验证用）
  tests/
    circuits.hpp         单测与交叉验证共用的电路
    simcore-tests.cpp    145 项断言 + --emit-verilog / --trace
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

`tools/simcore-iverilog.ps1` 把下面七个电路导出成 Verilog（`assign #(tplh,tphl)`、
`always @(posedge clk) q <= #ctoq d`），用 iverilog 13.0 跑一遍，再和内核自己的逐事件表
按"每个时间点、每根网"逐一比对。当前结果：**7 个电路全部一致**。

| 电路 | 覆盖的语义 |
|---|---|
| `delay_chain` | 每时序弧的 tplh/tphl、首次赋值同样走延迟、**惯性**脉冲滤波（窄脉冲被吞） |
| `multi_driver` | 多驱动解析：同意→该值、0/1 同强度→X、弱驱动输给强驱动、驱动关断→Z |
| `gate_unknown` | Z 输入按未知处理、0 主导 AND、1 主导 OR、常量 tie-off |
| `ring` | 三反相环释放后的振荡，周期 = 6 × 每级延迟（无 delta 环就做不出来） |
| `delta_chain` | 64 级零延迟链在同一时刻按 delta 轮收敛（递归实现会爆栈） |
| `dff_shift` | NBA 层语义 + clock-to-q：同一时刻读不到正要驱动的值 |
| `bus_split` | 4 位 Maker/Splitter 的位序与向量网解析 |

单测覆盖波形看不到的部分：强度解析表本身、调度顺序 `(time, layer, seq)`、
时间轮与溢出堆两条路径、过去时刻入队计数、setup/hold 违例、零延迟振荡报错而不是死循环、
自环反相器停在 X（不误报振荡）、未驱动网报告、VCD/Verilog 文本形状。

## 边界

- 器件库目前是 S1 规模：`buf/not/and2/and3/nand2/nand3/or2/or3/nor2/nor3/xor2/xor3/xnor2/xnor3/mux2/dff`
  加 `const`、`splitter`/`maker`。**没有的器件返回空指针、需要在接入层被报成"未支持"，
  不允许近似**（计划红线 6）。
- 时序库（S5）只有数据结构与查表入口，还没有文件格式；未命中时退化成"每器件一个常数"。
- 与游戏的对接（网表构建、槽位写回、推进控制）属于 S2，尚未开始；`simcore` 不知道游戏存在，
  这一点不能反过来破坏。
- VCD 的时间刻度是 `1ps`，一个 tick = 1ps，和 iverilog 导出的时间轴同一套坐标。
