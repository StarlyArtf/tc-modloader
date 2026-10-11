# 原版仿真 JIT：栈值复用与位域传播（2026-10-09）

目标：继续优化同一份原版 Overture 存档，争取达到 20 MHz。
本轮基线是已安装的 register-flow v6，而不是更早的 narrow-mask 版本。

- 基线 DLL：`build/tc-loader-sim-stack-baseline-v6.dll`
- 基线 SHA256：`1C02695D6BAC53924FFE06819F3C7B20C002EB7E39E2DED895B6D3FA476401F5`
- 冻结存档：`build/sim-round2-overture.data`
- 存档 SHA256：`54ACC8BBF8DE84E8A04D4F6C189BB1B2BE61A63F2085CA6417E040D0E4C0897E`
- 真实用户存档不修改；实际游戏测试使用独立克隆。

## 候选 v7

1. 跟踪直线代码中寄存器的实际值身份，移除目的寄存器已经具有相同值的完整 64 位复制。部分寄存器写入会使身份失效，32 位复制不直接当成 64 位复制。
2. 跟踪无索引 `RSP + 常量偏移` 栈槽。之前的同宽度读取或 64 位写入证明值仍然存在且该地址可读时，后续读取可使用尚未改写的寄存器。只处理完整寄存器写入的 MOV 和 MOVZX；不跨宽度转发。精确覆盖范围的栈写使重叠槽失效，其他间接写、未知指令、控制流边界、RSP 改写会清除槽信息。
3. 把常量左移、右移、算术右移纳入可能非零位的传播。移除已有机器码中的重复立即数 AND，前提是数据范围已经符合掩码且所有算术标志在被观察之前完整覆盖。保留 Boolean 的 MOV 0 初始化。

未改变循环计数、游戏显示速度、线程调度、存档电路、共享状态访问或 TC-Verilog 的编译方式。

离线捕获同一份原始代码（83,459 字节，入口 8,495）：

| 指标 | 已安装 v6 | 候选 v7 |
| --- | ---: | ---: |
| 输出字节 | 64,286 | 63,165 |
| 删除指令 | 5,230 | 5,453 |
| 冗余掩码 | 78 | 254 |
| 栈值转发 | 0 | 9 |
| 间隔等值复制 | 0 | 24 |

代码减少不等同于实测速度提高；安装与结论以真实克隆游戏测试为准。

## 正确性验证

- 先新增真实 x64 执行测试并确认旧实现失败，然后加入实现。
- 原有 JIT 测试通过。新增实际分配栈帧上的别名、字节重叠、间接写入、栈指针变化、分支汇合、跨宽度测试。
- 常量 shift：3 种操作 × 2 种宽度 × 7 种计数 × 1,000 个随机值，比较结果及定义的算术标志。
- PUSHF、SETcc、ADC 标志观察者阻止掩码删除。
- fast：simulation-hotpath、compatibility-contract、sdk-abi，3 项通过；`build/sim-stack-v7-fast-suite-20261009/results.json`。

v7：4,096 × 64 状态一致，CSV SHA256 为 `0C988F75C94A9C88378D8EA34589D32054B6C86F41BAD3E89FE48F36BFE0BE0C`；证据 `build/repro-ef2882eb565c4e499e15c06d9de5bc4a`、`build/repro-884668098ae04dfdb7cb5258e6f8ca73`。波形 30 行 / cycle 0–29 / 0 缺口：`build/scope-stack-v7`。运行时三次 setter、源码数 5→5、状态读回正确：`build/punch-tape-playtest-23f466209a304428a32ff714c4c9586f`。

## v7 速度对照

同冻结存档，默认 CPU 调度，测量窗口内没有并行编译或重测试。DLL SHA256：`431443D5B66C1E5CDA2222699D99C5B4EF4822113EF9FBF367DC4B2A77941155`。

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| 1 | 已安装 v6 | 15.2107 |
| 2 | 候选 v7 | 16.0235 |
| 3 | 候选 v7 | 16.2794 |
| 4 | 已安装 v6 | 14.7736 |

平均 **14.9922 → 16.1514 MHz（+7.73%）**，证据 `build/native-cpu-benchmark-c6c3da465a0b478e878b6ffc5c5f592d/results.json`。当前运行条件下的基线低于上一轮报告，绝对 MHz 不跨批次直接比较。

## v8 有符号扩展

在 v7 基础上，把 `(x << N) SAR N` 的 N=32/48/56 合并为 MOVSXD/MOVSX16/MOVSX8。完整 64 位结果相同，保留最终 ECX，沿用原始跳转入口与 flags 守卫。REX.W 与 REX.R/B 保证 BPL 和 R13 等源/目标正确，32 位目标与 AH 源的位域也有独立传播规则。

修复审查发现的 wrapper 遗漏：只替换读取、没有删除其他指令时也采用优化代码。新增 forward-only 场景中 removed=0 / specialized=0 / stackForwards=1 的真实执行测试。

新增 signed 执行对照先 RED 后 GREEN：5 寄存器 × 6 有效/等效计数 × 1,000 随机值；flags 观察者、部分入口、不匹配计数及非 8/16/32 位拒绝测试通过。所有旧 JIT 测试仍通过。

输出 **63,013 字节**，removed=5,491、constant shifts=139、narrow masks=474、dead constants=1,295、dead copies=727、redundant masks=254、stack forwards=9、equivalent copies=24、signed extensions=38。同一内层循环静态指令 **2,171 → 2,053**，SHL/SHR/SAR **166 → 90**（另新增 38 条 MOVSX），RSP 地址引用 **564 → 560**。静态条数不是每周期实际执行条数。

- DLL SHA256：`D3BE46AFC5FBE659D71EF054E4C6F698465A0A583B8BB5E241C34AF058CD818A`。
- fast 三项通过：`build/sim-stack-v8-fast-suite-20261009/results.json`。
- 4,096 × 64 状态相同且 CSV 哈希与 v7 相同：`build/repro-4bb5f2102cd642e6ab5c7d79d4a68174`、`build/repro-f58c954731864e4e958bf09123c79ca1`。
- 波形 30 行 / cycle 0–29 / 0 缺口，7 组加法器输入向量匹配：`build/scope-stack-v8`。
- 首次运行时常量回归未通过结束断言：`build/punch-tape-playtest-fef98d497d514445a11e23ea1e970808`。三次 setter 与状态读回已经正确，但导航加法器尚未记录 finished 就发生了运行时探针的主动重编译。此轮与其他两个游戏克隆并行；原参数串行复测通过，证据 `build/punch-tape-playtest-170e431558254d25bc874f9e976385ca`：cycle=39 / verdict=0、三次 setter、源码数 3→3、状态读回 1515870810。保留首次失败证据，未修改仿真代码或放宽断言。
- 两次独立只读审查未发现新增错算问题，wrapper 的集成遗漏已修正。

## 最终 v8 与已安装 v6 的直接对照

默认调度、新/旧/旧/新四轮；测量窗口内没有并行编译或其他重测试：

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| 1 | v8 | 16.7150 |
| 2 | 已安装 v6 | 16.7882 |
| 3 | 已安装 v6 | 16.5123 |
| 4 | v8 | 17.6597 |

平均 **16.6502 → 17.1873 MHz（+3.23%）**，证据 `build/native-cpu-benchmark-6b6e103e9f8749a9bc0056ede79dfc3a/results.json`。首轮新版与旧版接近，不能仅凭最后一次较高的读数宣称稳定的大幅提升。

随后直接比较 v7 与 v8，以确认最后添加的有符号扩展融合是否值得保留：

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| 1 | v7 | 17.2272 |
| 2 | v8 | 17.7242 |
| 3 | v8 | 18.2880 |
| 4 | v7 | 17.3008 |

平均 **17.2640 → 18.0061 MHz（+4.30%）**，证据 `build/native-cpu-benchmark-d8471f2fbc014a09868fc77a0b6a0cad/results.json`。v8 两次都高于 v7，两边实际 JIT 日志均核验，v8 的 signed extensions 非零。保留 v8。不同批次的百分比不相加、不相乘；最终两次约 18 MHz，**20 MHz 尚未达到**。这次也没有充分独立分解栈转发、等值复制和掩码传播各自的速度收益。

## 安装

2026-10-09 已把 v8 安装到 `D:/p/game_engine.dll` 和 `dist/tc-loader.dll`，两者与构建文件哈希均核验为 `D3BE46AFC5FBE659D71EF054E4C6F698465A0A583B8BB5E241C34AF058CD818A`。

安装前确认用户的真实游戏没有运行，两份旧安装文件仍是本轮起点 v6，冻结测试存档哈希未变。没有终止用户游戏或写入真实存档。

旧版备份：

- `D:/p/game_engine.dll.bak-20261009-before-stack-optimization`
- `build/tc-loader-before-stack-optimization-20261009.dll`

两份备份 SHA256 均为 `1C02695D6BAC53924FFE06819F3C7B20C002EB7E39E2DED895B6D3FA476401F5`。重新打开游戏即可手动测试。
