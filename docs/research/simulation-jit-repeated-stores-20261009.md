# 原版仿真 JIT：重复栈写入（2026-10-09）

继续优化原版通用仿真，以已经安装的 v9 为基线，仍使用冻结 Overture 电路。所有真实游戏测试在克隆目录完成，不写入真实存档，不停止用户游戏。

- 基线：`build/tc-loader-sim-store-baseline-v9.dll`，SHA256 `4C650FFD7AFAFB07DC7C3EF3D3BA6918684586CB07A0DF1C7EB706F1559B1BB7`。
- 候选 v10：`build/tc-loader-sim-store-v10.dll`，SHA256 `170B0EE4D62ED5D912FF474D28A8681C194DEA7004E460A7483A90F1E1A125B3`。
- 电路：`build/sim-round2-overture.data`，SHA256 `54ACC8BBF8DE84E8A04D4F6C189BB1B2BE61A63F2085CA6417E040D0E4C0897E`。

## 证明范围

现有直线区域内的值 ID 分析，已经可以证明固定 RSP 偏移的精确栈值。这次只删除完整 64 位 MOV store，且必须同时证明：该八字节位置有先前保留的实际 store；当前内容与待写源寄存器值相同；RSP 和控制流证明没有失效。

区分“内容未变”和“先前已写成功”。已知间接写入可能改变内容，因此清空内容记录；它不会改变先前已写过的八字节访问范围。后续真实 load 可以读取新的内容，随后把这个新值原样写回才可删除。仅有 load 从不建立可写证明。

部分覆盖会使相关内容记录失效；未知指令、CALL、控制流入口和 RSP 写入清空相关证明；不跨这些边界删除。不改共享状态写入、字节/32 位 store、flags、游戏计数、调度或电路。已有宽度精确匹配和寄存器别名规则保留。

## 验证进度

重复 store 和别名改值后 load/store 两个执行测试，分别观察到 RED→GREEN。新增负例执行验证部分覆盖、间接别名、RSP 变化、条件入口、未知指令/CALL、源值变化、32 位零扩展、首次仅有读取证明的 store；既有机器码测试仍全部通过。

同一原始 JIT：83,459 字节，v9 62,756 → v10 **62,671 字节**；新增删除 **11 次重复 store**，入口 6,341 → 6,317。其他删除/转换计数保持不变。热循环 v9 29111..38901 为 2,005 条，v10 29082..38816 为 **1,998 条**；RSP 引用 560 → **553**，其中 7 次新增删除在主循环。SHL/SHR/SAR 为 90 条，MOVSX/MOVSXD 为 7 条，都没有变化。

构建成功，只有此前已存在的 3 条警告。fast：simulation-hotpath、compatibility-contract、sdk-abi，**3 通过 / 0 失败**；结果 `build/sim-store-v10-fast-suite-20261009/results.json`。

真实状态对照通过：v9/v10 的 **4,096 周期 × 64 通道逐项相同**，CSV SHA256 `6266B1CA49451B1447D796E9358025B07CC13E3F6CD76618B05406FD9506DDA8`，也与上一轮同配置记录一致。对应克隆 `build/repro-9235cd461f494e678c07b5a885b1e4bf`、`build/repro-018d270c448e4921a7a162341a42507e`。

波形回归通过：`build/scope-store-v10`，cycle 0..29 连续 30 行、64 通道、0 缺口，7 组已记录加法器输入/输出符合预期。

常量运行时回归通过：`build/punch-tape-playtest-1a673e40e03a4dcb92f060b239d0943e`，3 次 setter、源码数量 3 → 3、状态偏移 310 等于 1515870810；autotest cycle=39、verdict=0。未覆盖抽屉 UI 点击。

独立只读审查完成，未发现需要修复的安全/语义问题。核查了实际保留 store 才建立可写性、内容与可写性独立失效、间接及重叠写、真实 load 更新值 ID、32 位清高位、删除地址的目标、未知/CALL/RSP、后续 pass 保留证明来源 store，以及 ABI 和缓冲生命周期。

## 速度对照

速度以同批基线/候选交错测试的周期增量 / 墙钟时间决定，不凭代码变短安装。

v10 / v9 第一组 ABBA：`build/native-cpu-benchmark-13ed9307cd1247cb854e5009528ff931/results.json`。

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| A1 | v9 | 20.01144476 |
| B1 | v10 | 20.01216954 |
| B2 | v10 | 20.08966036 |
| A2 | v9 | 19.76005950 |

平均 19.88575213 → 20.05091495 MHz，+0.8306%。首个相邻 A/B 基本持平，收益较小，没有仅凭这组就安装 v10。进一步收紧 byte store 的位需求，构建 v11 最终候选。

## v11：字节存储位需求

在已有字节 CMP/TEST 的位需求处理内加入 opcode 0x88 且 ModRM 为内存的 byte store。数据源只消费真正的 AL/AH/R9B 等字节，地址基址/index 仍消费完整 64 位；同一物理寄存器同时作数据源和地址时，需求合并为完整地址需求。只细化输入位需求，不删除字节内存写，不改变 AND 的 flags 覆盖守卫。

新增 byte store 执行测试观察 RED→GREEN，负例与执行覆盖包括 AH、完整 64 位 store、R9B + 1,000 随机值、指针本身作为源（清 bit 8 时目的地址变化，真实分配页内核对写入位置），以及 SETcc 读取 mask flags。

v11 相对 v10 又删除 13 条掩码，unusedMasks 13 → 26，重复 store 仍为 11 次。原始 83,459 → v11 **62,586 字节**，入口 6,317。v11 热循环地址 29043..38771，共 **1,997 条**；其中主循环相对 v9 合计删 8 条（7 次 store、1 条掩码），RSP 引用 553，移位 90，符号扩展 7。13 条新增掩码删除多数在这段主循环之外，不能按总删除数推算循环提速。

v11 DLL：`build/tc-loader-sim-byte-store-v11.dll`，SHA256 `9960DE0F0C6F808330457375BFBD6FA5BA9C591167A15EAE7861383B1CBBD821`。构建成功，仍为 3 条已存在警告；fast 3 通过 / 0 失败，`build/sim-byte-store-v11-fast-suite-20261009/results.json`。

v11 对 v9 的 **4,096 周期 × 64 通道逐项相同**，CSV 哈希仍为 `6266B1CA49451B1447D796E9358025B07CC13E3F6CD76618B05406FD9506DDA8`。克隆为 `build/repro-b00d1236fd634ec0b2460fc97130306a`、`build/repro-8d34f88c7f2d4a4ba7933a79925eb3ba`。

v11 波形回归：`build/scope-byte-store-v11`，连续 0..29 共 30 行、64 通道、0 缺口，7 组已记录输入/输出符合预期；容量 30 行满后 trace_rows=31。

v11 独立只读审查完成，未发现必须修复的问题；检查实际字节映射、完整基址/SIB 索引需求、同寄存器数据和地址需求合并、未知前缀、保留 store 副作用、固定点和 flags 守卫。

v11 常量运行时回归通过：`build/punch-tape-playtest-36dd85cdbb974b9bbb14731df44adfe1`，3 次 setter、源码数量 3 → 3、状态偏移 310 = 1515870810、autotest cycle=39 / verdict=0。未覆盖抽屉 UI 点击。

v11 第一组 ABBA 未完成：`build/native-cpu-benchmark-2ac37e7d6e4a4ab99323343b99e3d44b/results.json` 只保留前两次有效记录，v9 20.04967414、v11 20.06043640 MHz。第三次候选克隆 `build/repro-0fb51c4bbe2a4992aeb6cd8788eb6d74` 在 t=20..37 窗口只有 t=20.2995、21.3131 两条记录，触发原有少于 10 样本的拒绝条件，不计入收益。fault.log=0 字节、autotest cycle=39 / verdict=0，正常大电路 JIT 完成，初始化第一份 JIT 的文件时间与前两次接近；尚未确定该轮后续帧采样停顿的外部原因，不能说是已证明的启动延迟或仿真错误。

反序 BAAB 重跑，将克隆总生命周期从 43 延长到 60 秒，仍使用原来的 t=20..37 采样窗口和至少 10 样本守卫；不放宽状态、周期回退或 JIT 转换检查。完整通过，`build/native-cpu-benchmark-162159b79d89459198e79d7bd46e528d/results.json`。

| 顺序 | 版本 | MHz |
| --- | --- | ---: |
| B1 | v11 | 18.95859533 |
| A1 | v9 | 18.90246722 |
| A2 | v9 | 18.59908505 |
| B2 | v11 | 19.00332662 |

同批平均 **18.75077614 → 18.98096098 MHz，+1.2276%**。不同批次绝对速度存在明显波动，不把 v10 的 +0.8306% 与这组收益相加，也不把不完整 ABBA 当成完整收益证据。本轮结果支持小幅改善，未证明稳定达到 20 MHz；需要用户在原来的游戏环境手动确认。

## 安装

保留两项已校验的通用优化，最终安装 v11。`D:/p/game_engine.dll`、`D:/p/tc-modloader/dist/tc-loader.dll` 均复制后核实为 `9960DE0F0C6F808330457375BFBD6FA5BA9C591167A15EAE7861383B1CBBD821`；真实游戏没有运行，未停止用户进程。

v9 备份：`D:/p/game_engine.dll.bak-20261009-before-repeated-stores`、`build/tc-loader-before-repeated-stores-20261009.dll`，两份均核实为 `4C650FFD7AFAFB07DC7C3EF3D3BA6918684586CB07A0DF1C7EB706F1559B1BB7`。冻结电路哈希仍为 `54ACC8BBF8DE84E8A04D4F6C189BB1B2BE61A63F2085CA6417E040D0E4C0897E`。
