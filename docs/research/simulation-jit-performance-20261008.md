# 原版仿真 JIT 优化（2026-10-08）

在此前空沙盒与动态常量优化基础上，增加一层保守的 x64 机器码优化。此次针对游戏原版仿真编译器，不改 TC-Verilog 生成器、速度显示或仿真批次算法。

## 实现

`src/simulation_jit.hpp` 解码游戏生成的程序，移除完整 64 位寄存器的自复制、相邻逆向复制中的冗余第二条，以及 `mov ecx, 0` 后的零位移指令。32 位写入的高位清零语义、内存操作、非零位移和算术标志位保留。

代码压缩后，重算全部直接相对跳转/调用以及执行入口。分支能直接进入的第二条复制或零位移不删除。不支持的编码、无效目标、IP 相对寻址（包括地址宽度前缀）、间接跳转、x87 指令和 call-next 获取 PC 的程序整体使用原始字节；准备阶段异常也回退。`TC_MODLOADER_JIT_OPT=0` 可在启动时关闭这一层。

加载器使用游戏已有 JIT 入口：游戏复制私有字节序列到可执行内存后执行，私有输入缓冲在执行返回前保持存活。核对了游戏 2.1.334 的入口实现，其编译完成回调为空，没有另行持有旧代码偏移的元数据。现有兼容性约束继续生效。

冻结原版 Overture CPU（111 个实际元件、展开后 125 个节点）的生成程序从 83,459 字节减至 79,310 字节，移除 1,383 条指令；入口从 8,495 重定位至 7,862。循环内移除 253 条逆向复制和 10 条零位移。

## 测速

同一加载器、同一冻结电路与 Mod 包，按关闭、开启、开启、关闭的顺序运行。每组独立游戏与克隆存档，关闭波形录制。以日志的浮点时间和周期增量计算运行约 20–37 秒期间的 MHz；禁止原测试 runner 再发送控制指令，并拒绝周期重置。开启组均有实际 JIT 优化日志，关闭组没有。

| 顺序 | 新优化 | MHz |
| --- | --- | ---: |
| 1 | 关闭 | 5.702 |
| 2 | 开启 | 6.294 |
| 3 | 开启 | 6.324 |
| 4 | 关闭 | 5.393 |

关闭平均 5.547 MHz，开启平均 6.309 MHz，提升约 **13.7%**。关闭组有约 5.6% 的组间波动，单对比较约为 10–17% 提升；两次开启结果接近。结果覆盖这一实际原版 CPU，包含游戏渲染和测试驱动开销，不能据此承诺所有电路或 RV32I 的相同增幅，也不等于界面瞬时显示值。

复现：`tests/native-cpu-benchmark.ps1 -Loader build/tc-loader-sim-jit-v2.dll -Circuit build/sim-round2-overture.data -VerilogMod ../mods/example.tc-verilog-ram.mod`。结果为 `build/native-cpu-benchmark-fe7f2822cf724fe0bab5f6b1a1beb676/results.json`，其中保存四组原始日志目录、时间与周期。

旧 pin-shape 测速驱动的第一组发生周期重置，该批测速不作为此处的性能证据。渲染线程逐帧读取组合缓存也不能可靠判断逐周期等价，最终状态比较采用仿真线程的周期结束采集。

## 验证

- 实际执行机器码测试：10,000 组随机输入的值和算术标志位对比；零位移、32 位写入、分支进入候选指令、跳转重定位和非零入口，以及不支持代码回退均通过。
- 实际原版 CPU：周期 0–4095、状态字节 256–767 的 64 个 64 位通道全部相同。两份完整 CSV 的 SHA-256 为 `77D784B965E93B9AC5A0DFFB276D5C2A36740CC49706EDEA0B731A81C5BFF0E7`。验证文件 4096 行、每行一个周期加 64 个值。采集有一次 -1→0 的初始重建，此后无缺口。
- 真实游戏波形采集：30 行，cycle 0–29，0 缺口；输入符合加法器向量，通过。
- 真实游戏在线常量：三次 setter 更新，源码计数保持 5→5，状态读回 `1515870810` 与期望一致。使用 `punch-tape-playtest.ps1 -RuntimeOnly` 覆盖 setter→JIT→状态链路，未验证抽屉鼠标点击；证据为 `build/punch-tape-playtest-c0b87639ab594242a09f7062152e00db`。
- 快速测试：`simulation-hotpath`、`compatibility-contract`、`sdk-abi` 三项通过，报告为 `build/sim-jit-fast-suite/results.json`。未声称仓库所有测试全绿。
- 代码审查通过；修正了 IP 相对寻址前缀的回退条件，并强化真实游戏测试对实际启用优化及完整 CSV 写入的断言。

状态复现：`tests/native-cpu-state-playtest.ps1 -Loader build/tc-loader-sim-jit-v2.dll -Baseline build/tc-loader-sim-round2-baseline.dll -Circuit build/sim-round2-overture.data -VerilogMod ../mods/example.tc-verilog-ram.mod`。冻结电路是本机测试文件，不随源代码发布；测试始终只修改克隆存档。

## 构建

构建：`build/tc-loader-sim-jit-v2.dll`，SHA-256 `CAE04669FEC9DAA4367B8263D3E2CDFE9692F7C13537741922D11B465B8E3275`。

前一轮已安装版本 SHA-256：`3ECFA0CC83A65F9C0488B18DAD7C0E49EE5C729D587AAD23CBC84145BE0A7E8F`。

已同步安装至 `D:/p/game_engine.dll` 与 `dist/tc-loader.dll`，逐一核验 SHA-256 与构建一致。上一版备份为 `D:/p/game_engine.dll.bak-20261008-before-jit-opt`，dist 备份为 `build/tc-loader-before-jit-opt-20261008.dll`。重启游戏生效，真实存档未修改。

强化后的状态测试再次通过，原始文件位于 `build/repro-8c50442068ac46b383fd41e5c5e14f82`（基线）与 `build/repro-bfeffb78ed9c4b19aa847fd53333ef76`（候选）各自的 `game/tc-modloader-data/plugin-data/dev.pin-shape-probe/cpu-state.csv`；哈希仍与上文一致。
