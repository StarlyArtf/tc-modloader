# 原版仿真寄存器流优化（2026-10-08）

目标为用户确认的首次测试存档 Overture，冻结电路 `build/sim-round2-overture.data`，111 个实际元件、展开 125 个节点，SHA-256 `54ACC8BBF8DE84E8A04D4F6C189BB1B2BE61A63F2085CA6417E040D0E4C0897E`。本轮继续优化游戏原版生成的 x64 机器码，目标 20 MHz；不改速度显示、仿真周期计数或 TC-Verilog 生成器。

## 实现与约束

新增 `src/simulation_registers.hpp`，对已识别的 x64 指令记录物理寄存器读取、完整写入、局部写入及可能置位的位。未识别编码、操作数/地址覆盖前缀、调用、返回和控制流停止证明；所有原始跳转/调用/入口目标都是边界，包括之前删除的 no-op。

- 删除常量赋值：目的寄存器在被读取之前已被完整覆盖。32 位写入清除高 32 位；AL/AH、SETcc 和带 16 位前缀的写入不当作完整覆盖。内存的基址、SIB 索引和存储源都算读取。RSP 不参与删除。
- 删除死寄存器复制（v5 候选）：同一证明扩展到规范的 64 位寄存器 MOV，不删除内存读取。单独记录 `dead copies`，防止只运行到旧规则而误报新优化生效。
- 删除直接跳往下一条指令的空跳转（v6）：只匹配 E9/EB 的零位移规范编码，269 处、其中原热点循环 82 处。它本身不形成汇合边，但其他分支、调用及显式入口指向该位置时仍保留目标保护；重定位映射到下一条保留指令。不删除 LOOP、条件跳转或 CALL-next。
- 传播可能置位的位：MOVZX、32 位写入、立即数 AND、AND/OR/XOR 等形成保守的上界；只省掉此前安全掩码融合生成、而且输入已经满足位宽限制的 AND。仍要求标志位随后被覆盖。
- 扩大标志位覆盖扫描：64 → 256 条原始指令，继续在标志位读取、调用、返回及未知指令前停止；内存 ADD/SUB/CMP 同样可证明覆盖算术标志。实际 Overture 掩码融合从 260 增至 423 处，单独常量移位从 140 增至 203 处。

曾尝试删掉高位已知为零的 MOV0→SETcc 初始化（v2/v3）。语义测试通过，但没有证明独立性能收益；最终候选 v4/v5 保留初始化。不能将指令减少直接等同于速度提升。

## 候选和测速

原安装版：`1DDB69A64F577A4E94599BC51C1A34512073C2A7527E77F677F354081CDEB29F`，另存 `build/tc-loader-sim-register-flow-baseline.dll`。所有游戏运行均使用隔离目录、同一冻结电路和 `../mods/example.tc-verilog-ram.mod`，双方已有 JIT 优化均启用。以 t≈20–37 秒的周期增量计算 MHz，拒绝周期重置，并检查真实编译日志中候选规则命中。

| 候选 | 新增内容 | Overture 字节数 | SHA-256 |
| --- | --- | ---: | --- |
| 原安装版 | 上轮窄位宽合并 | 76,893 | 1DDB69A6… |
| v1 | 死常量 + 已窄值掩码 | 71,404 | 3E7DD4DB58776CA1DAEF7BCBA668C6B9C8CD09A5B54F4615045864599509601D |
| v2 | 另删布尔初始化 | 70,275 | 2650D4C6604A42FC0260A9EB82DC7578227A8DBAA084EFC52E7B743CDDD20FC1 |
| v3 | 扩大标志位扫描 | 67,461 | B8CF38D77E9AA1511DBABA73764BEDF46065E35B4A5D8C39DE2E83051F12F4E3 |
| v4 | 保留布尔初始化 | 68,595 | B08896D27AFF461D1276C37287E2BF589A853098371BF4ADF6F6039D88705E5E |
| v5 | 另删死寄存器复制 | 66,207 | 20998411DB10AA60754BB5E1DEA43C932BE35444C8F7E302E4CDE34DC8EEFC4C |
| v6 | 另删空跳转 | 64,286 | 1C02695D6BAC53924FFE06819F3C7B20C002EB7E39E2DED895B6D3FA476401F5 |

v1 首轮 ABBA：旧版 12.5117、11.2169，新版 13.5366、13.2029 MHz，均值 11.8643 → 13.3698（+12.69%）。证据 `build/native-cpu-benchmark-ee482c02ae9e49bca39353f9f47eb09b/results.json`。

v2 ABBA：旧版 10.8040、7.9998，新版 9.9413、11.3453 MHz。基线本身波动较大，不据此计算可靠提升，也不宣称布尔初始化删除变快或变慢。证据 `build/native-cpu-benchmark-b09773f7a189487cab0aa871dde7265e/results.json`。

本机 i7-13650HX 的 Windows CPU Sets 枚举显示 logical 0–11 为 EfficiencyClass=1、12–19 为 0。新增可选 `CpuAffinity` 仅控制隔离测试进程，用于排查调度变量；产品未修改线程亲和性，也没有证据认定核心迁移就是上述波动的原因。固定 0xfff 的两次对照：旧版 11.5276 → v3 12.7228 MHz（`build/native-cpu-benchmark-89ee1af4cc3c4a52ae555f2bff414961`）；v3 14.2503 → v4 14.3838 MHz（`build/native-cpu-benchmark-56a136949b0e4771888527184c4158a6`）。后一对不足以证明布尔初始化带来稳定改善。CPU Sets 的字段依据 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-system_cpu_set_information)。

**v4 默认调度下 BAAB 四次对照**：

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| 1 | v4 | 14.2144 |
| 2 | 原安装版 | 12.3844 |
| 3 | 原安装版 | 12.3588 |
| 4 | v4 | 14.2835 |

平均 **12.3716 → 14.2489 MHz（+15.17%）**，双方两次结果接近。证据 `build/native-cpu-benchmark-26a03621dbdd4e8ca32e2cf8f37bd73f/results.json`。此组测速时未并行编译或运行其他重测试。界面截图在测量窗口后采集，显示约 14 MHz；目标 20 MHz 尚未达到。截图 `build/sim-register-flow-v4-screen.bmp`。

## 正确性与诊断工具

- 机器码执行测试按 RED/GREEN 新增。包含随机值/标志位、CH 与 REX.BPL、SIB 与 REX.X、基址读取、部分写入、32 位清高位、SETcc 标志保留、内存 CMP、长 store 段、标志位观察、入口/分支/调用和短跳转回退。
- v1、v2、v3、v4 均校验 4,096 周期 × 64 个 64 位状态通道，与原安装版完全一致。CSV SHA-256 为 `0C988F75C94A9C88378D8EA34589D32054B6C86F41BAD3E89FE48F36BFE0BE0C`。v4 证据 `build/repro-2dd0b452a79646f2aaef17466c8601b0`、`build/repro-34f7ebc63c46403c8894eab9abdab32b`。
- v4 快速检查 3 项通过、0 失败：`simulation-hotpath`、`compatibility-contract`、`sdk-abi`，报告 `build/sim-register-flow-v4-fast-suite/results.json`。未声称仓库全部检查通过。
- v4 运行中常量：三次 setter、源码数量 5→5、状态读回 1515870810 正确，证据 `build/punch-tape-playtest-a3462c0535174778a3a04ed80f61ebb7`。只覆盖 RuntimeOnly，未验证抽屉鼠标操作。
- v4 波形：30 行、cycle 0–29、0 缺口，7 组已记录加法器输入向量匹配，证据 `build/scope-register-flow-v4-fixed2`。
- 首次波形测试失败：旧导航探针只通过抽屉 UI 取得 model，未进入 byte_adder，停在 sandbox、rows=0。新增测试专用 wrapper 通过 Board 句柄取得 model，包装同一加法器实现，只在 sandbox 内重新打包并执行 CLI apply 更新包哈希。原有周期/向量断言保留。没有修改正式加法器 Mod；初次改包未更新启用清单时被哈希防护拒绝，已补充隔离目录 apply。
- 独立只读复核覆盖 JIT 与测试工具变化，已修复 PowerShell 7 空环境变量触发诊断亲和性失败的问题。

## 最终候选与安装

v5 校验 4,096 × 64 状态一致，波形 30 行/0 缺口，运行中三次常量更新无重编译，三项快速检查通过。没有单独测速，不宣称死复制删除的独立收益。证据为 `build/repro-453ae952154740049e4055e8c9d10cfb`、`build/scope-register-flow-v5`、`build/punch-tape-playtest-115e064023ca4b1fb2a9cbfd5fcc531b`、`build/sim-register-flow-v5-fast-suite`。

v6 当前总计删除 5,230 条，单独常量移位 215 处、掩码融合 474 处、死常量 1,333 条、死复制 745 条、重复掩码 78 条、空跳转 269 条；原始 83,459 → 64,286 字节，入口 8,495 → 6,404。内层循环静态指令从原始 3,143 降至 2,171，移位 334 → 166，但栈地址引用仍是 564。分支存在，静态条数不是每周期实际执行条数。

v6 正确性已确认：4,096 × 64 通道状态与原安装版一致，证据 `build/repro-97579a69db6542df9eb47afde616aac5`、`build/repro-eb9d56a3aa4549f7914a48fb0d94704e`，CSV 同为 0C988F75…；`build/sim-register-flow-v6-fast-suite` 三项快速检查通过；`build/scope-register-flow-v6` 波形 30 行/cycle 0–29/0 缺口；`build/punch-tape-playtest-b2ec94b276de45c9b8394a24b9a0e045` 三次 setter 生效、源码数 5→5、读回 1515870810 正确。独立复核未发现必须修复的 JIT 问题。

v6 与 v4 两点对照为 13.1608 → 15.6671 MHz，证据 `build/native-cpu-benchmark-ead5bb6f412e49ab8bf52d54840a175c/results.json`。没有单独分解死复制和空跳转各自的速度收益。随后一组四轮对照被用户中断，目录 `build/native-cpu-benchmark-19a0ceaf4c9c409cabcddab309593a67` 未留下完整结果，不计入性能证据。

### 2026-10-09 最终对照与安装

继续任务后重新执行三项快速检查，通过 3 项、失败 0 项，报告 `build/sim-register-flow-v6-fast-suite-20261009/results.json`；重新核验原版和 v6 的保存状态 CSV，两者哈希仍为 0C988F75…。直接比较本轮开始时的安装版和 v6，默认 CPU 调度、同一冻结存档，按新/旧/旧/新运行：

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| 1 | v6 | 16.5168 |
| 2 | 原安装版 | 12.4365 |
| 3 | 原安装版 | 12.4616 |
| 4 | v6 | 16.6424 |

平均 **12.4490 → 16.5796 MHz（+33.18%）**，两版各两次结果接近。数据 `build/native-cpu-benchmark-e29d29a92f69411a93ae3cd4e3c802ff/results.json`，真实编译日志确认 `fallthrough jumps 269` 命中。测量窗口中没有并行编译或运行其他重测试。界面截图在窗口后采集，显示 **17 MHz**，文件 `build/sim-register-flow-v6-screen.bmp`。**20 MHz 目标尚未达到**，本次平均收益不代表所有电路，也不能直接换算玩家其他运行条件下的绝对 MHz。

已将 v6 装入 `D:/p/game_engine.dll` 和 `dist/tc-loader.dll`。两者与构建文件哈希均核验为 `1C02695D6BAC53924FFE06819F3C7B20C002EB7E39E2DED895B6D3FA476401F5`。安装前确认真实游戏没有运行，并且两份安装文件仍是本轮基线。

备份为 `D:/p/game_engine.dll.bak-20261009-before-register-flow` 和 `build/tc-loader-before-register-flow-20261009.dll`，均核验为原安装版 `1DDB69A64F577A4E94599BC51C1A34512073C2A7527E77F677F354081CDEB29F`。真实存档未修改，重新打开游戏生效。
