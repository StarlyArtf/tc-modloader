# 版本历史

## 0.8.5（2026-10-11）

- 加载器内置原版仿真的 x64 JIT 优化：消除冗余寄存器复制、常量移位、位宽掩码与重复栈写入；按控制流和实际使用的位保守分析，不能证明安全时保留原程序。默认启用，可用 `TC_MODLOADER_JIT_OPT=0` 关闭额外 JIT 优化作对照。
- 未录制波形时跳过采集外调；可编辑宽常量直接读取稳定原子槽，在线修改仍即时生效。冻结 Overture 电路的 4096 周期、64 通道对照一致。实际 MHz 随电路、机器和运行负载变化，不承诺固定达到 20 MHz。
- TC-Verilog 增加模块展开、参数、generate、组合块和逐输出依赖分析；符合条件的纯组合设计可直接生成游戏 JIT 表达式，并复用公共子表达式。
- 修复原版 Keyboard 在选通脚未连接时不输出键码，以及事件之间丢失按键状态的问题；保留真实连接的选通条件。
- 扩展自定义元件的编辑器、组合网表、引脚通道与运行时生命周期接口；提供 Mod 设置、持久存储及 TC-Verilog 源码编辑服务。SDK ABI 基线同步更新。
- 更新单窗口安装器和相关验证；源码包补齐 simcore 与安装器图标，发布产物包含确定性压缩包和 SHA-256 校验。
- 发布回归修复：已识别的旧版加载器可直接卸载并恢复原始引擎；精确依赖元件的内部接线避免单依赖锥、门输入、宽输入干线及分数引脚间距误连。源码包同时包含两个独立示例构建脚本，并可在没有预生成 native 目录的情况下构建；旧版 PowerShell 下安装器中文文件描述保持正确。

以下按开发日期记录各项改动的实现细节；本版本之前的“未发布”条目是历史开发记录。

## 未发布：通用仿真热路径（2026-10-07）

- 未录制波形时，生成程序直接读取采集开关并跳过 `tc_scope_tick` 外调；开始录制后仍逐周期采集。
- 9–64 位可编辑常量直接读取永久原子槽，CYCLE/REFRESH 不再逐拍调用 DLL、访问 TLS 或查哈希表。
  在线修改仍即时生效，既有 setter 与读取桥 ABI 保留；诊断源码保留常量 ID 以便追溯状态槽。
- 新增 `simulation-hotpath` 快速回归，并让电路复现脚本可指定加载器、Mod 包和冻结电路，支持隔离对比。

## 未发布：原版 Keyboard 元件的输出修正（2026-10-06）

- 游戏自己生成的程序里，Keyboard（kind `0x5d`）的行为现在由加载器在编译前改写。玩家报的现象
  是"元件明明收到了按键，却不会输出键码和按键状态"，两条原因都在那段生成代码里：读取脚
  （`in0`）没有被驱动时，游戏把条件编译成恒假的 `if is_new_value_3 && (U1 0x0) != 0`，弹出
  分支永远不执行；而 else 分支用 `is_new_value << 16` 覆盖输出缓存，把刚读到的按键状态与键码
  一起清掉。两份程序用 `TC_MODLOADER_DUMP_SOURCE=1` 抓下来逐字比对过（见
  [diagnostics](reference/diagnostics.md)）。
- 改写只动 `com_keyboard` 程序块里的这三点：选通脚被编译成常量 0（`(U1 0x0)` / `(U1 0)`）时
  去掉该条件，改为"有事件就读"；玩家真正接了信号的选通脚保持原样（例如
  `&& (U1 ((U1 vid256))) != 0` 不动），录制的按键序列仍按周期确定性地回放。else 分支只刷新
  "有事件待读"那一位，保留缓存里的状态+键码，输出因此保持最后一次事件直到下一条到来，而不是
  在两事件之间塌回 0。else 分支还要**把本周期局部变量（按键状态、键码）从缓存里恢复**：
  游戏把生产者的局部变量直接交给同一周期内的下游，而不是让它读已发布的槽位，所以少了这一步，
  锁存只有"看板显示"看得见——下游（例如写使能常开的 Store 端口 `store(..., (U8 vid276))`）
  只在读到事件的那一拍拿到键码，下一拍就用 0 覆盖回去，内存里永远留不住按键。
- 原版 kind、引脚、贴图与调色板条目全部不变，也不改游戏 EXE/DLL：改写挂在加载器已经持有的
  生成源码拦截点上（`src/native_logic.hpp` 的 `compile`，与注入 `tc_scope_tick` 同一处），
  改写器独立在 `src/keyboard_fix.hpp`。`TC_MODLOADER_KEYBOARD_FIX=0` 关闭它（对照用）。
- 证据分两层。离线：`tests/keyboard-fix.cpp` 用真机抓到的两段原文（未接选通、接了 Constant 的
  选通）以及出问题那块板子的原文（`store(..., (U8 vid276))`）做单测，断言三处改写、幂等、
  以及非 Keyboard 程序块不受影响。真机：
  `tests/keyboard-fix-playtest.ps1` 把一块 0x5d 板子当作 sandbox 关卡的电路载入（刻意不加载
  `dev.sandbox-sim`，因为它会抑制游戏自己的 run），由 `tests/keyboard-fix-driver.cpp` 按游戏
  自己的队列布局（条目 `down|key<<8`、步长 8、回绕 4096、写指针在 +4098，与
  `simulator_queue_key_down/_up`、关卡脚本的 `add_keyboard_value` 同一套）注入按下/抬起事件，
  再经 `sim.do` 请游戏跑几拍并从 state 数组读回输出槽：修正后 `out1=1 out2=0x41`（按下）→
  `out1=0 out2=0x41`（抬起）；`-Disabled` 对照跑同一块板、同一组事件时是
  `out0=1 out1=0 out2=0x0`——缓冲区里有事件、元件也说"有事件待读"，键码和状态就是上不了脚。
  用例 `keyboard-fix` 与 `keyboard-fix-control`，驱动包 `dist/dev.keyboard-fix-driver.mod`
  仅供测试，不进发布清单。

## 未发布：TC-Verilog 组合核直接进入游戏 JIT（2026-10-06）

- V2/V3 定义尾部新增版本化 `TC_COMPONENT_TYPE_INLINE_EXPRESSIONS` 程序：宿主把每个输出的
  直线前缀与最终表达式复制进注册表，在游戏生成模拟程序时直接替换回调调用。组合热路径由此不再
  经过 DLL FFI、输入分块、`TCLogicIOV3` 打包或 AST 解释器。
- TC-Verilog 会把无持久状态、全驱动的二态组合设计降成 `U64` SSA。支持 RV32I flat 核使用的
  连续赋值、组合块、if/case、拼接/复制/选择、算术、逻辑、比较与移位；内存、锁存、四态、复杂
  signed 运算和时钟状态仍保守回退原 V3 回调。
- 每个输出从共享 SSA 图反向裁剪，只保留最终值可达的节点，并按输出重命名。生成器同时验证程序
  只引用该输出依赖掩码内的输入，避免游戏重排输出节点时出现跨输出临时变量和假组合边。
- 依赖掩码相同的输出现在在游戏实际调度到该组的第一个输出时只发射一次公共 SSA 前缀；组内输出
  只保留各自的末端表达式，因此既能跨输出消除重复解码，又不依赖游戏选择的输出执行顺序。
- inline 降低器进一步保留立即数、把整宽单驱动 wire 与无符号扩宽变成别名，并省掉可证明冗余的
  位宽掩码；Mux 从两次乘法改成 `no ^ ((yes ^ no) & (0 - cond))`，常量条件和布尔 Mux 直接折叠。
- 条件先布尔化并由 CSE 共享；`cond ? x : 0`、`cond ? 0 : x` 和单比特置位/清零使用专门的短公式。
  所有输出还改为从一张规范 SSA 图切片：即使依赖掩码不同，指令译码等公共节点也保持相同名字，
  加载器按游戏实际输出顺序逐声明发射一次。精确依赖和拓扑顺序不变，寄存器数据尚未产生时只会先
  发射纯指令前驱，读出之后再发射依赖寄存器值的 ALU 节点。
- 两端均为动态值的通用 Mux 改成 JIT 局部变量加条件赋值，不再无条件执行 XOR/AND/取负选择公式；
  常量门控继续使用无分支短公式。加载器按完整声明块去重，因此多行条件节点也只在首次使用处出现。
- 单比特复制（尤其立即数符号扩展里的 `{{20{instr[31]}}, ...}`）直接降成一次取负和位宽掩码，不再
  生成逐份移位/OR 链；常量拼接、前导零扩宽、静态位选/移位及常见算术/位运算恒等式同步折叠。
- inline 输出在 CYCLE 模式下是永久驱动的单一源，加载器现在直接发布其值与 `zid=true`，不再经过
  通用 Switch 的 OR/AND 合并、冲突检测与 Z 比较。跨行生成时用括号深度流式跳过整个死块；定位
  Switch 使能条件也从目标 `vid` 声明之后开始，避免误改 inline 前缀中 Mux 自己的 `if`。
- 运行时可编辑常量改为地址稳定、永不回收的原子槽。模拟线程缓存最近一次 `component id -> slot`
  映射，逐拍读取只剩 relaxed atomic load，不再获取全局 mutex 或重复查询哈希表；低频 setter 与
  既有 `tc_dynamic_constant` ABI 不变，纸带/常量在线编辑无需重编译的语义保留。
- `rv32i_flat.v` 已得到 13 路 inline 程序：`state_words=0`，优化后注册文本约 138 KiB。真机沙盒
  成功编译，生成源由最初约 307 KiB / 2688 个 SSA 临时量降到 117,012 B / 690 个，乘号由约
  590 个降到 5 个；只编译 CYCLE/REFRESH 两种模式，没有回调 FFI，注册日志带 `jit=inline`。
  440 条 RV32I 自检仍以 `0x5a5a5a5a` 签名通过。
- ABI 快照扩展到 1890 条；旧定义的字段偏移不变，未覆盖新尾部或未设置标志时行为不变。

## 未发布：TC-Verilog 逐输出切片、组合拓扑与临时状态（2026-10-05）

- 精确依赖输出的生成代码现在把目标输出编号交给 V3 回调；TC-Verilog 为每个输出预编译反向组合切片，
  REFRESH 只执行所需 `always` 块并只生成这一只输出，不再为每个引脚重复解释完整设计。
- 组合块在编译期建立读写图。无环网络按稳定拓扑序执行一次；只有含组合环的切片退回最多 8 轮的
  既有不动点路径。时钟块在边沿提交前先执行自己的组合前驱切片，因此 RHS 使用当前输入而不是上拍
  留下的中间值。
- 单一组合写者、全路径整寄存器赋值、无自反馈且不在组合环中的变量迁入每次调用的 scratch；不完整
  赋值形成的锁存、多写者、存储器和环内变量仍在实例 state。RV32I 内核版的持久状态从 62 word 降到
  34，纯组合 flat 版从 28 降到 0。
- 隐藏 tick 现在只执行时钟 RHS 前驱与边沿提交，不再重算公开输出。连续输出切片按组合块实际读取的
  叶子信号复用纯 scratch 结果；锁存器、存储器、架构状态写入和组合环不参与缓存。
- 新增显式 `TC_COMPONENT_TYPE_GROUPED_OUTPUTS`：TC-Verilog 把依赖掩码相同的输出合成一次 V3 回调，
  组首先发布整组，后续驱动只回读。`rv32i.v` 的 9 个输出归成 `0x0`/`0x2` 两组，因此一拍从
  10 次回调降到 3 次（2 组 + tick）；普通 V2/V3 定义不设置标志时行为不变。
- 时钟事件另有一份只含敏感信号的边沿计划：无边沿时不再先解释完整 RHS 数据流；确认至少一个块触发
  后才执行时钟前驱。块缓存命中改为原位比较/更新，scratch、解析标记、触发位与 NBA 写队列复用实例
  缓冲，不再在每块/每回调分配临时向量。
- 模拟真实精确依赖桥（无关输入为 Z）的离线 RV32I 基准中，交替时钟的内核版约 **363 kHz**；
  静止/下降沿 tick 约 **0.1 us**，包含上升沿的交替 tick 平均约 **1.05 us**（只含回调，不含游戏
  棋盘/RAM 调度）。语言测试新增逐输出隔离、组选择、tick-only、块缓存失效、锁存保留与组合环退化断言。

## 未发布：内壳布线：门的间距与逐锥分段（2026-10-05）

- **问题（玩家报的"循环依赖"）**：`component_definition::encode` 的精确路径把一个锥内的门
  **一行一个**排下去，而 XOR 的输入在 row±1：于是第 k 个门的 "+" 脚和第 k+2 个门的 "-" 脚落在
  **同一格**。一个输入的抽头线终止在那一格，下一级门的前级线也终止在那一格，游戏把它们并成一根
  网——两个输入被短路，网上出现两个驱动，游戏随即判**组合环**（元件工坊里还能看到那条线从门上
  横穿过去）。真机复现：`foundry/Verilog RAM 16x8` 的定义解出 18 节点/18 线，其中
  `wire5+wire11 @(-4,0)`、`wire6+wire13 @(-4,8)`、`wire9+wire12 @(-1,-5)`、`wire11+wire14
  @(-1,-4)` 合并，网 9 有两个驱动，环 `0→5→11→12→11`。
- **修法**：锥内门距改成 **4 行**（行距 3 已能分开引脚，第 4 行留给走线），每个锥独占一段行带
  （带间留 3 行），前级的线改从驱动器旁的空列绕到下一个门的 "-" 脚，不再横穿门体。节点
  **数量与顺序一字未动**（契约只固定顺序），桥的解析与既有注册路径不受影响。
- **新工具**：`tools/definition-check.py` 把生成的定义解回节点/走线，报告同锚点节点、端
  点落在别的线上、一个网多个驱动、组合环，并打印每个输出脚可达的输入脚集合——后者才是真正
  要对的量：它必须等于声明的依赖掩码。
- 真机：重装加载器后 `foundry/Verilog RAM 16x8` = **0 问题**（原来 16 问题/2 环），
  `时钟源` = 0 问题，RAM 的输出脚可达 {2,3} = 声明的 `0xc`。回归 `native-component`、
  `tc-verilog-project`、`component-compose` 全绿。
- **仍存在的边界（下一步）**：①**只有一个依赖的输出**不生成门（节点数由桥在
  `native_logic.hpp` 里按掩码算出的 `dependencyGates` 公式固定），它的抽头走到驱动器时要从 x=3
  横穿门列，落到那里任何门上——
  真机上 `foundry/rv32i`（13 输出）仍是 5 驱动 + 组合环；②输入脚 ≥8 时抽头列会跑到元件外并穿过
  输入脚列。正解是让行带整体落到所有驱动器行之下（那片区域无门），再给每个带门的锥一条自己的
  竖走廊，或直接改用 `component_netlist::planRoutes` 接管这段布线——两条都还没做。

## 未发布：Mod 元件也能在元件工坊里打开看接线（2026-10-05）

- **问题**：工坊打开元件时是拿**名字**去玩家存档里找
  `<存档>/schematics/foundry/<名字>/circuit.data`。组合元件早有这条（2026-09-29 起），回调型
  Mod 类型没有，于是"在元件工坊中编辑"只会开一张白板——玩家存档里那些 49 字节的空定义就是它
  （真机：`foundry/RV32I` 与 `foundry/rv32i_single_cycle`，各 49 字节、带随机 id）。
- **加载器注册时补上这条**：新函数 `provideFoundrySchematicBytes` 把类型自己生成的那份定义写进
  工坊条目，V1 `register_component`、V2/V3 `tc.component.types`、以及 `tc.component.runtime`
  的运行期注册与替换四条路都接上。工坊由此画出的就是这颗元件真实的内壳：引脚、每个输入的收集器、
  逐输出的依赖锥、驱动，以及它们之间的导线——和元件栏缩略图用的是同一份字节。
- **必须同时接受"游戏已经导入过它"**：游戏在**插件加载之前**会把 `schematics/foundry/` 下的一切
  当成玩家元件导入，所以下一次启动时，这些条目已经带着同一个 custom id 躺在原型表里，普通导入
  会被 `-3 that id is already imported` 拒绝（实测：`local.clock` 连插件初始化都失败）。现在
  加载器会先核对工坊条目里确实是本类型自己的 id，是的话走 `update_custom_prototype`
  **就地替换**（删掉可见原型再加不回去——游戏把 custom id 存在不止一个索引里，这正是 update
  入口存在的原因）；不是我们的条目、或玩家自己改过的同名片，仍然按原样拒绝/不动。
  `importGeneratedDefinition` 把这套"导入或就地替换"收成一处，V1、V2/V3、复合元件三条路共用。
- **不动玩家的东西**：定义以 custom id 开头，条目里带本类型 id 的才算"我们写的"，可以刷新；缺失或
  ≤64 字节的游戏占位才覆盖；玩家自己改过的同名片一律不动（沿用组合元件那套判据）。
- 组合元件原先的实现改成调用同一个函数，行为与字节不变。
- **按钮**：Mod 仍可用 render V4 的 `setFoundryButton(..., false)` 隐藏按钮（`local.float-ops`、
  `local.text-box` 在这么做）。要连这些一起看，设 `TC_FOUNDRY_SHOW_MOD_TYPES=1`，宿主会让那些
  隐藏请求在这一局失效并记一行日志。
- 真机：`tests/live-root-workshop.ps1` 拉起实机（隔离 profile）后 `loader.log` 出现五行
  `foundry view: filled …`——`Verilog RAM 16x8`、`rv32i`（tc-verilog，13 个输出各带依赖掩码）、
  `时钟源`/`自锁开关`/`按钮`（local.clock）；文件内容与 `component_definition::encode` 的输出
  逐字节一致（735 / 2055 / 4925 字节，头部 `0e …` + custom id）。第二遍真机把"条目已存在"的
  profile 直接拿去过启动：五个类型全部 `registered custom …`，`local.clock` 正常加载，
  再没有 `-3`。回归 `byte-adder`、`tc-verilog-project`、`component-compose` 全绿。

## 未发布：时钟模块的精确依赖与隐藏 tick（2026-10-05）

- 带 `always @(posedge …)` / `negedge` 的 TC-Verilog 现在也发布逐输出依赖。公开输出仍按自己的
  掩码在 REFRESH 中独立求值；一个**没有外部输出**的隐藏全输入汇点在 CYCLE 中恰好调用一次，
  负责提交 PC、寄存器堆等时序状态。这样既不重复提交状态，也不会把 tick 的全输入锥画成任何
  `输入 → 公开输出` 组合边。
- 新增 `TC_COMPONENT_TYPE_CLOCKED_TICK`。旧定义和不设置该标志的调用方保持原外壳；TC-Verilog
  在 `Program::hasClockedState()` 为真时自动同时设置它与精确依赖标志。
- 根治精确 V3 多输出的第二个缺陷：每个输出和隐藏 tick 的生成代码现在使用唯一的
  `tc_zin` / `tc_is` / `tc_io` 临时名。此前重复声明会让游戏 JIT 编译失败，并在界面上表现成
  "循环依赖"与持续重编译，即使依赖图本身已经正确。
- 真机门禁 `tests/pin-shape-playtest.ps1 -Shape clocked` 已改为 V3：3 进 2 出、精确掩码、隐藏
  tick，`full_adder` 上 `calls = cycle+1`、状态每周期只加一且输出正确。另用玩家的
  `D:/p/rv32i/rv32i.v` 和原板复现：注册表为 `0x0,0x2,…`，只生成 4 份正常源码，模拟推进到
  cycle 51，不再反复编译。

## 未发布：逐输出的组合依赖（"假组合环"的根治，2026-10-05）

- **问题**：TC-Verilog 生成元件的内壳把**每个输出**都画成依赖**全部输入**。外部寄存器堆因此被
  游戏看成一个环：`寄存器堆读数据 → 内核 → 读地址 → 寄存器堆 → 读数据`。Verilog 源码里
  `assign rs1_addr = instr[19:15];` 明明不读寄存器堆，但游戏只看生成出来的电路拓扑，于是单周期
  CPU 被判成循环依赖。
- **编译器**：`sdk/tc_verilog.cpp` 新增逐输出依赖分析——对 `assign` 和组合块做"输入掩码"不动点
  传播（位选/部分选择的基准、`if`/`case` 的控制条件都算依赖），`Program::outputDependencies()`
  给出每个输出实际读了哪些输入引脚，`Program::hasClockedState()` 标出模块里有没有边沿块。
- **定义 ABI（向后兼容）**：`TCComponentTypeDefinitionV2`/V3 末尾追加
  `const uint32_t* output_dependencies` + `output_dependency_count`，并新增
  `TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES` 标志。`size` 停在旧尾部的定义一个字节都不用改，
  继续走保守路径；加载器会校验"每个输出一个掩码"且掩码不引用不存在的输入。
- **加载器内壳**：`component_definition::encode` 拿到掩码后为**每个输出**生成一棵私有依赖锥
  （输入收集器 + 该输出自己的依赖门 + 驱动），不再是一条吃全部输入的链；`native_logic` 按同一张
  表给每个驱动单独打包**它自己**的输入，并用 `tc_logic_output` 独立求值。注册日志随之打印
  `deps=0x1,0x2,...`，真机上可直接对照游戏看到的依赖图。
- **时钟模块**：公开输出依旧逐项 REFRESH；额外隐藏 tick 每周期只提交一次状态。时序模块和纯组合
  模块因此使用同一张精确图，而状态提交不会成为公开组合边。
- 回归：`tests/tc-verilog.cpp` 直接编译 `tests/fixtures/rv32i_single_cycle.v` 并断言
  `imem_addr` 只依赖 `pc`、`rs1_addr`/`rs2_addr`/`rd_addr` 只依赖 `instr`、`rd_wdata` 依赖全部五个
  输入；`tools/verilog-dependencies.cpp` 可离线打印任意模块的这张表；真机
  `tests/tc-verilog-deps-playtest.ps1` 在沙盒里启动游戏，断言加载器收到同一张表。

## 未发布：原生逻辑总输入扩到 2048 位（2026-10-05）

- `tc.component.types` 的 V2/V3 元件现在最多可声明 **32 个输入、32 个输出，输入总宽度
  ≤2048 位**；单脚仍是 1–64 位。旧 V1 `TCLogicIO[8]` 的 ABI 没变，因此仍是最多 8 脚、
  总输入 ≤512 位。
- ≤128 位继续走原来的 `invoke(token, cycle, lo, hi)` 快路径。更宽的输入不增加 JIT 外调参数，
  而是用若干四参数 `stage` 调用按 128 位分块，完整、周期匹配的批次到齐后才执行一次回调；
  V3 的 value 与 drive mask 分平面暂存，CYCLE/REFRESH 也各自隔离。周期编码带一位偏置，因此游戏
  初始化阶段的 cycle=-1 也能完整传输。
- 输出状态槽从每实例 512 字节扩到 2048 字节，32 个 64 位输出互不重叠；32 个实例的最高槽仍低于
  已钉住版本的模拟状态边界。
- 离线回归覆盖 32×64 位最大形状、缺块拒绝、V3 三态驱动平面、cycle=-1 和输出槽边界；真机
  `tests/pin-shape-playtest.ps1 -Shape wide2048` 注册 32×64 位消费者，并从第 32 路输入把
  `0x0123456789ABCDEF` 原样送进回调，证明第 1985–2048 位实际经过游戏 JIT 与桥接层。

## 未发布：TC-Verilog 页面里的源码编辑器（2026-10-05）

- **源码框换成了真正的编辑器观感**：行号装订区（光标所在行高亮）、Verilog 语法着色
  （关键字、`$系统任务`、`` `编译指令 ``、带位宽／基数的数字、字符串、行注释与块注释）、
  回车按上一行缩进并随 `begin`／`case`／`else` 进一级、随 `end`／`endcase`／`endmodule`
  退一级，Tab 仍插入真正的制表符。光标、选区、鼠标拖选、撤销重做与 IME 还是引擎自己的
  `InputTextMultiline`，插件没有第二份编辑状态。
- 新增 `sdk/tc_code_editor.h`（`tc::ui::codeEditor` / `CodeEditorState` /
  `loadCodeEditor`）。它是**增量**能力：任何一条必需导出缺失都会退回普通多行输入，并在
  加载器日志里写明原因；调色板属于控件自己，不读游戏样式表。
- 页面打开时自动把焦点交给源码框（和 IDE 打开文件一样），这同时是编辑器切到"自己重画全部
  文本"的前提：多行 `InputText` 的文字画在它自己创建的子窗口里，而 ImGui 渲染子窗口晚于
  父窗口，父窗口上叠加的着色文字会被输入框自己的文字盖住。回调结构验证通过后，控件把
  `ImGuiCol_Text` 推成全透明并自己着色重画（光标色与选区色不受影响）；验证前只叠加关键字
  颜色，最坏情况是"颜色淡"，不会变成空白框。
- `Ctrl+S` 在源码框里同样保存（等同"保存源码"：先编译验证，结构没变就热加载，变了就排队替换
  运行模型）。切换文件时控件 id 带文件序号，因此每个文件有自己的编辑会话与撤销栈。
- 回归：新增 `tests/tc-code-editor.cpp`（`build\tc-code-editor-test.exe`）覆盖 Enter 缩进
  判定、Tab 缩进保留、多字符编辑不动缓冲区、非法事件与短缓冲区被拒绝、没有引擎时报告不可用；
  真机回归 `tests/tc-verilog-project-playtest.ps1` 抓整页截图后按调色板数像素（关键字 ≥ 500、
  数字 ≥ 60 个纯色像素），把"截图里真的有高亮"变成自动判定。

## 未发布：TC-Verilog 的层级名字与 generate if/case（2026-10-05）

- **层级名字（只读）**：展开实例的信号可以用实例路径读到——`assign y = u0.sig;`、嵌套
  `u0.u1.sig`，以及 generate 循环里同名实例的 `u0[1].sig`（第 0 个也接受 `u0.sig`）。
  解析顺序是"本作用域/本模块的名字优先，其次才是层级路径"，所以 `a[0]` 这类普通位选不受影响；
  层级名字只允许出现在表达式右侧，赋值会报 `a hierarchical name may only be read, not assigned
  to`，找不到的路径报 `unknown hierarchical name 'u0.nope'`。实现是实例展开时记录
  "实例名.信号名 → 符号 id"（连同该实例自己展开出的层级路径一起加前缀），因此它读到的是真实
  的、活的信号（测试里读到了子模块内部的 `shadow` 寄存器与输出 `q`，并逐拍核对数值）。
- **generate 的 `if` / `else if` / `else` 与 `case` / `casez` / `casex`**：条件与标签必须是常量
  （通常用参数，`#(.P(v))` 的覆盖会一路传到条件里）。**所有分支都会被语法检查，只有被选中的
  那一支进入设计**：未被选中的分支在解析后被完整回滚（符号、连续驱动、状态字、时钟块、层级表、
  常量绑定），所以两个分支可以声明同名信号、驱动同一根 net，互不冲突；`discard` 走的是"先全部
  回滚、最后再解析被选中那支"的顺序，避免被丢弃分支的驱动/声明与选中分支互相干扰。`default`
  写在哪一条都一样，只在没有标签命中时生效。
- 内核单元测试新增四组真值断言：`if (WIDE)` 两支分别被选中（同一个子模块用 `#(.WIDE(0))`
  覆盖后走另一支，行为随之改变）、`if (WIDE)` 默认参数那支、`case (MODE)` 的 `default` 写在
  最前面仍只在无标签命中时生效、层级读（`u0.shadow` 与 `u0.q`，`stateWords()==3`，逐拍核对
  NBA 语义）；另加三条拒绝路径断言（非常量 generate 条件、未知层级路径、给层级名字赋值）。
  `build/probe-verilog-grammar.exe` 里"generate if（未选中分支里有实例）""generate case"
  "层级读"三行从 `REFUSED` 变成 `ACCEPTED`，非常量条件与未知路径各自给出对应诊断。

## 未发布：TC-Verilog 的展开体作用域（2026-10-04）

- 每个展开的循环迭代、以及 `generate` 里的每个 `begin/end` 块现在都是**独立作用域**：标识符
  解析先查打开的作用域栈（由内向外）再查模块级名字；作用域里声明的东西记在隐藏键下
  （`t#7` 之类）并用源名字登记进作用域，所以
  - 同一名字可以出现在不同迭代里——它们是互不相干的信号；
  - 块内声明**遮蔽**同名外层信号，出了块就看不到（引用报未知信号）；
  - 同一个块里重复声明同名仍然是 `duplicate declaration of 'x'`。
  这样"每个实例一份内部连线/寄存器"就能写了：生成体里 `wire t; sub u(.a(a[g]), .y(t));
  assign y[g] = t;`，或 `reg r; always @(posedge clk) r <= d[g];`——每个迭代各有自己的线与
  寄存器（各自占状态字，如"每位一个寄存器"的用例是 4 个寄存器 + 4 个时钟字 = `stateWords()==8`）。
- 入口随之补上：`generate … endgenerate` 现在用**生成块**解析器（`parseGenerateBody`）处理每一项，
  所以 `generate begin … end endgenerate`（不带循环、直接一个块）也是合法的，并拥有自己的作用域；
  之前这种写法会报 `unsupported module item`。
- 子模块展开（实例化）时作用域栈随其它解析状态一起保存/恢复，子模块内部的声明仍然只属于它自己
  的符号表——否则子模块的端口会解析不到，这一点由既有的实例化回归守住。
- 内核单元测试新增三组真值断言：生成体里每轮声明 `wire t` 并实例化（`0b0101 -> 0b1010`）、
  每轮一个 `reg r` 的"每位一个寄存器"（`stateWords()==8`，逐拍采样与 `en` 保持）、块内声明
  遮蔽外层同名（`y[0]` 走块内的 `t`、`y[1]` 走模块级的 `t`）；另加两条拒绝路径断言
  （作用域名字出了块就报未知信号、同一块里重复声明）。`build/probe-verilog-grammar.exe` 里
  "生成块内声明"与"每轮一个寄存器"两行从 `REFUSED` 变成 `ACCEPTED`，"块外引用作用域名字"
  报 `unknown signal 'inner'`。

## 未发布：TC-Verilog 的组合式 always（2026-10-04）

- 新增**组合式过程块**：`always_comb`、`always @(*)` / `always @*`、以及电平敏感表
  `always @(a or b)`（`or`/`,` 都认）。块体里用**阻塞赋值 `=`**，时钟块里仍必须用 `<=`；
  一边用错会在源码位置报 `use '=' in a combinational block` /
  `blocking assignments are not supported; use <= in clocked logic`。`always_latch` 仍然拒绝
  （不推断锁存），`always_ff` 配电平表报 `always_ff needs a posedge/negedge sensitivity list`，
  `always_comb @(...)` 报 `always_comb takes no sensitivity list`。
- 执行模型：组合块**不占敏感表状态字**，结果写进被赋值变量自己的状态字；每个宿主阶段
  （CYCLE/REFRESH/RESET）都求值到**不动点**（上限 8 轮）。所以①一个块里 `t = a & b; y = t | c;`
  这种阻塞链立刻可见；②两个块互相读对方的结果与**声明顺序无关**；③寄存器被时钟块提交之后，
  组合逻辑同一拍就看到新值（CYCLE 里先跑时钟块的提交、再跑组合不动点），REFRESH 单独跑也能
  反映当前状态。成环时取最后一次迭代的值（Verilog 会是 X），这一点写进文档。
- 新增两条写入检查：同一个变量不能既被组合块又被时钟块赋值（`'x' is assigned from both a
  combinational and a clocked block`）；组合结果里出现 X/Z 时整条赋值被跳过（状态字存不下 X，
  沿用既有策略并在文档里说明——需要三态请用 `assign` 驱动 wire）。
- 顺带把时钟块的敏感表状态字改成**在解析到该 always 时分配**（原来放在模块体解析完之后），
  这样从子模块展开进来的块不会在父模块里再拿一次状态字。因为引用必须先声明，被 always 块用到的
  reg/存储器一定写在它前面，所以既有设计的布局逐字不变（计数器 2、RAM 17 的断言照旧）；
  只有"always 块之后才声明、且块里没用到"的变量会挪位置，这一种代码会在重编译后换布局。
- 内核单元测试新增三组真值断言：`always_comb` 的二选一（`stateWords()==1`，光靠 REFRESH 就能
  切换输出）、跨块顺序无关的网络（`y` 的块写在 `t` 的块前面，仍读到新 `t`；`always @(a)`
  电平表也用上）、组合块读寄存器（时钟沿后 `qbar = ~q` 立刻跟上新值，REFRESH 同样正确）；
  另加四条拒绝路径断言（组合块里用 `<=`、两种块写同一变量、`always_ff` 配电平表、
  `always_comb` 带敏感表）。`build/probe-verilog-grammar.exe` 里 `always @(*)` / `always_comb`
  / `always @(a or b)` 三行从 `REFUSED` 变成 `ACCEPTED`（`state=1`，无敏感表状态字）。

## 未发布：TC-Verilog 的模块实例化（2026-10-04）

- 一个源文件可以写**多个 `module`**：`run()` 先词法扫出每个模块的 token 区间，再挑出顶层
  （**最后一个没有被任何模块实例化的 module**，判定是"模块名 + 实例名 + `(`/`#`"的词法扫描；
  全部都被实例化时取最后一个），用顶层重新走一遍单模块解析。原来"`endmodule` 之后不能再有
  内容"的限制随之取消。
- **实例化在编译期展开**：`<模块> [#(.P(v))] <实例名> (连接…) ;` 会以该模块的 token 区间**重新
  解析**一次（新的符号表、新的状态布局），然后把结果并入父模块——符号 id 因为追加而整体偏移
  一个常量（重映射就是加偏移），寄存器/存储器的 `stateOffset` 挪到父模块的状态数组里，敏感表
  的状态 word 同理。因此嵌套实例、逐实例参数覆盖、以及"同一个子模块用不同参数实例化两次"
  都走同一条路径；每个实例都是独立的寄存器副本（`stateWords()` 相应对得上）。
- 连接支持：具名 `.a(expr)`、具名简写 `.a`、空连接 `.a()` 与位置空槽 `(a, , y)`（不驱动的
  输入读作 Z）、位置连接 `(expr, expr)`；**输出端口**可以接整根 wire，也可以接常量边界的
  位选/部分选择，所以 `generate for (g = 0; g < W; g = g + 1) leaf u(.a(a[g]), .y(y[g]));`
  成立。输入端口变成一根由连接表达式驱动的 wire；输出端口变成驱动父模块那根网的一段
  （重叠驱动仍按"该位已有连续驱动"拒绝）。
- 参数覆盖只接受具名形式 `#(.P(v))`（`.P` 简写也可，取同名常量）；覆盖一个模块没有的参数、
  位置形式 `#(8)`、未知模块名、未知端口名、输出接到 `reg`、顶层之外的多余连接、递归实例化
  都有各自的诊断。顺带修好一条误报：`always_latch`/`task` 这类不支持的模块项不再被当成
  "未知模块名"。
- 内核单元测试新增五组真值断言：两级 `mux2`+`top`（具名连接、`moduleName()=="top"`、行为）、
  位置连接的有状态计数器（子模块的 reg 与时钟字进入父模块状态，`stateWords()==2`，逐拍计数与
  `en` 保持）、`ones #(.N(8))` / `#(.N(2))` 两个实例不同参数（`0xFF` / `0b11`）、
  `generate for` + 每位一个 `inv` 实例（位选连接，`0b1010 -> 0b0101`）、`.a` 简写与空连接
  （`0 & Z = 0`，`1 & Z` 不是确定值）；另加六条拒绝路径断言（未知模块、未知端口、输出接 reg、
  位置参数覆盖、覆盖不存在的参数、同一网两处驱动）。

## 未发布：TC-Verilog 的有符号算术（2026-10-04）

- 值（`Bits`）新增 `isSigned`，并贯通解析、常量折叠与求值：
  - **声明**：端口与信号可以写 `signed`（`input wire signed [3:0] a`、`output reg signed [7:0] q`、
    `reg signed [7:0] mem [0:3]`），`unsigned` 也接受；
  - **字面量**：`8'shff` / `4'sd2` / `'shff` 这类 `'s` 有符号常量（此前被拒为
    `malformed based literal`）；
  - **转换**：系统函数 `$signed(x)` / `$unsigned(x)`（新表达式种类 `Cast`，位与宽度不变、只改类型）；
  - **运算符**：`>>>` / `<<<` 进入词法（三字符，优先于 `>>`/`<<`）与优先级表。
- 语义按 IEEE 1364 的常用规则：放大位宽时 signed 值**符号扩展**（最高位是 X/Z 也扩展成 X/Z），
  否则补零；`+ - * / %` 与 `< <= > >=` 只在**两侧都是 signed** 时按有符号解释，只要一侧是
  unsigned 整个表达式就是无符号；`>>` 永远补 0，`>>>` 在左值是 signed 时补符号位（用补码
  算术实现，不依赖编译器对负数右移的实现定义行为）；`==`/`!=` 只比位模式；比较、拼接、位选、
  部分选择、复制的结果都是无符号；`/`、`%` 按 Verilog 向零截断，`INT64_MIN / -1` 这类边界
  显式处理，不触发 C++ 未定义行为。参数常量仍按无符号 32 位值参与折叠（文档写明）。
- 内核单元测试新增四组真值断言：signed 与 unsigned 比较同一组位（`1000` 小于 `0001` 只在
  signed 下成立）、放大时符号扩展 `0b1110 -> 0xFE` 与 `$unsigned` 后 `0x0E`、`>>>`/`>>`/`<<<`
  （`1110 -> 1111 / 0111 / 1100`）、有符号字面量与有符号除法/取负（`4'sd2`、`-2/2 = -1`、
  `-(-2) = 2`、`4'sh8`）、`$signed` 与混合类型表达式保持无符号；另加一条 `$signed` 缺括号的
  拒绝断言。`build/probe-verilog-grammar.exe` 里 `signed` 声明、`$signed()`、`>>>`、`8'shff`
  四行从 `REFUSED` 变成 `ACCEPTED`，`$display` 等系统任务仍拒绝。

## 未发布：TC-Verilog 的 parameter / localparam（2026-10-04）

- 新增**模块常量** `parameter` / `localparam`，两种位置都支持：模块头的
  `module m #(parameter W = 8, D = 4)(...)`（`parameter` 关键字可省，`#(W = 8, D = 4)` 也认）
  与模块体里的 `parameter P = ...;` / `localparam P = ...;`；可选 `integer`/`signed` 或
  `[range]` 类型被接受并忽略，只取折叠后的值。
- **这一刀把"参数化位宽"接通了**：`parseWidth` 与数组上下界从"只认十进制字面量"改成
  "接受常量表达式"，常量本身走与循环变量同一套 `ConstantValues` 环境（新符号种类
  `Parameter`，不占状态 word）。于是 `input wire [W-1:0] a`、`reg [W-1:0] mem [0:D-1]`、
  `a[N-1:0]`、`{N{1'b1}}`、`for (i = 0; i < N; i = i + 1)` 都能由参数决定，用途与限制同
  `docs/sdk/tc-verilog.md` 所述。常量在**声明处**求值，所以只能用先声明的常量（前向引用报
  未知信号），且不能被过程赋值。
- 这一版没有模块实例化，所以 `parameter` 的"可被上层覆盖"和 `defparam` 无从谈起：写下的
  默认值就是值，`parameter` 与 `localparam` 行为一致（文档里写明了这一点）。顺带删掉了因此
  失去唯一用途的 `parseDecimalToken`。
- 内核单元测试新增四组真值断言：`#(parameter W = 8, D = 4)` 的 RAM（端口位宽来自 `W`、
  `stateWords()==5`、写入读回 `0xA5`）、`localparam TOP = W - 1` 驱动循环边界与位选
  （`0b1100 -> 0b0011`，`{a,a}` 得 `0xCC`）、`{N{1'b1}}` / `{M{1'b1}}` 两个复制计数
  （`0xF` / `0xFF`，`stateWords()==0`）、模块体内 `parameter BASE` + `localparam STEP = BASE/2`
  的算术（`a+24`）；另加四条拒绝路径断言（前向引用、非常量初值、重复声明、给参数赋值）。
  `build/probe-verilog-grammar.exe` 里五种参数写法从 `REFUSED` 变成 `ACCEPTED`。

## 未发布：TC-Verilog 的 for / generate 展开（2026-10-04）

- 新增**编译期循环变量** `integer i;` / `genvar i;`（新符号种类 `LoopVar`，不占状态 word，
  也不能被过程赋值），以及 **`for` 循环**的两种用法：
  语句里的 `for (i = 0; i < 8; i = i + 1) q[i] <= a[7 - i];`，
  与模块级的（`generate` 里或直接写）`for (i = 0; i < 2; i = i + 1) assign y[i*4 +: 4] = ...;`；
  嵌套 `for` 也可以。
- **展开在编译期完成，循环变量折成字面量**：解析器保存 `for (…)` 头的三处 token 位置
  （一份词法扫描，不做试探性解析），每一轮把变量绑进 `LoopConstants` 再解析一次循环体，
  所以 `i` 能出现在位选 `q[i]`、部分选择、`[base +: width]` 的基准、拼接、`case` 标签和任意
  算术里，而生成的 AST 里只剩常量——运行期没有循环，也不需要为循环变量留状态。
- 边界必须是常量：起值、条件、步进都在绑定下折叠，折不出常量就报
  `the for loop's condition must be constant`（起值/步进同理）；步进必须写回同一个循环变量；
  单次编译的展开上限 **4096 轮**，死循环（如 `i = i`）在这里被拒绝；`generate` 循环至少要
  跑一次（语句循环可以跑零次，此时循环体仍会被解析一遍以确定词法范围）。
  循环变量逃出自己的循环报 `a loop variable may only be read inside its own for loop`，
  SystemVerilog 的 `for (genvar i = ...)` 就地声明报
  `declare the loop variable before the loop, e.g. 'integer i;' or 'genvar i;'`。
- 模块项解析重构出 `parseModuleItem` / `parseGenerateBody`，`generate`、`integer`/`genvar`
  与模块级 `for` 都走同一条入口；`generate … endgenerate` 与不带 `generate` 的模块级 `for`
  等价。还没做的：`if`/`case` 形式的 generate、generate 块名的层级引用、展开体里的信号声明
  （第二轮会报重复声明，因为每一轮没有独立作用域）。
- 内核单元测试新增四组真值断言：语句 `for` 做位反转（`0xC0 -> 0x03`，且 `stateWords()` 仍是
  2）、模块级 `for` + 索引部分选择做半字节交换（`0x12 -> 0x21`）、`generate for` 生成 4 个
  `always` 块（`stateWords()==5`，逐拍采样 `0b1010`/`0b0101`）、嵌套 `for`（`0x0C -> 0x03`）；
  另加五条拒绝路径断言（非常量条件、循环变量逃逸、SV 就地声明、零次 generate、死循环）。
  `build/probe-verilog-grammar.exe` 里五种 `for`/`generate` 写法从 `REFUSED` 变成 `ACCEPTED`。

## 未发布：TC-Verilog 的多事件敏感表与异步复位（2026-10-04）

- `always` / `always_ff` 现在接受**多事件边沿敏感表**：`@(posedge clk or negedge rst_n)`，
  `or` 与 `,` 两种写法都认，单独 `@(negedge clk)` 也可以。敏感表的每个条目各占一个状态 word
  记录上一拍电平，任何条目出现配置的边沿就执行块体：
  `posedge` = 上一拍不是 1、这一拍是 1；`negedge` = 上一拍不是 0、这一拍是 0（与 Verilog 把
  x→1、x→0 也算边沿一致）。单事件块仍只占 1 个状态 word，老模块的状态布局逐字不变。
- 于是 `always @(posedge clk or negedge rst_n)` 里的复位是**异步**的：`rst_n` 拉低那一下不需要
  时钟沿就执行块体；复位保持期间每个时钟沿依旧走复位分支；复位释放（表里没有 `posedge rst_n`）
  本身不触发。内核每个宿主周期只采样一次电平——是边沿检测而不是事件队列，所以异步复位在下一个
  宿主周期生效，这一点写在 `docs/sdk/tc-verilog.md`。
- 组合式的 `always` 仍然拒绝，但换成了明确的说法：`a sensitivity list entry must be 'posedge'
  or 'negedge'; a combinational always block is not supported yet`（`always @(*)` 与电平敏感表
  都走这一条），2 位以上的边沿信号报 `an edge-triggered signal must be one bit wide`，
  条目之间少了 `or`/`,` 报 `expected 'or' or ',' between sensitivity list entries`。
- 内核单元测试新增两组真值断言（异步复位：复位下降沿单独清零、保持期间时钟沿仍清零、释放不触发、
  `en` 低电平不动；`negedge` 单事件块 + 逗号敏感表两路触发）与三条拒绝路径断言；
  `build/probe-verilog-grammar.exe` 里 `always @(negedge clk)`、`or` 与 `,` 三种写法从
  `REFUSED` 变成 `ACCEPTED`（`state=2` / `state=3`），`always @(*)` 仍拒绝。

## 未发布：TC-Verilog 的 case / casez / casex（2026-10-04）

- 内核支持 **`case` / `casez` / `casex`** 语句：标签可写逗号列表（`3'd0, 3'd1:`），
  `default` 可以出现在条目列表的任意位置、且只在没有任何标签命中时执行，条目体可以是单条
  语句或 `begin/end` 块；`case` 与 `if` 一样可以在时序块里嵌套。
- 比较是**四态**的：普通 `case` 逐位精确比较（X 只匹配 X、Z 只匹配 Z，任一标签位与被测位
  一方未知而另一方确定就不匹配）；`casez` 把标签里的 `z` 和 `?`，以及被测值里的 Z 当“不关心”；
  `casex` 再把被测值里的 X 也当“不关心”。本内核的字面量把 `?` 存成 X，所以标签里的 X 位在
  `casez`/`casex` 里同样算通配（这是与标准唯一的、写在文档里的偏差）。
- 两条更清楚一点的诊断：`case` 少了 `endcase` 时报 `unterminated case statement; expected
  'endcase'`；条目缺语句时报 `expected a statement`，不再退化成“未知信号 'endcase'”。
- 内核单元测试新增三组真值断言（逗号标签 + default + 块体、`casez` 的 `?` 通配与 default
  前置、`casex` 接受全 Z 的被测值和拒绝不匹配的确定值）与三条拒绝路径断言（缺 `endcase`、
  两个 `default`、条目缺语句）；`build/probe-verilog-grammar.exe` 里 `case`/`casez`/`casex`
  三行从 `REFUSED` 变成 `ACCEPTED`，模块层的裸 `case` 仍然拒绝。

## 未发布：TC-Verilog 拼接、复制与位选/部分选择（2026-10-04）

- 内核支持 **拼接 `{a, b}`**（左起为最高位）、**复制 `{N{value}}`**（`N` 必须是常量）、
  **位选择 `x[i]`**（下标可以是变量）、**部分选择 `x[msb:lsb]`**（边界必须是常量）与
  **索引部分选择 `x[base +: width]` / `x[base -: width]`**（宽度是常量、基准可以是变量）。
  词法层新增 `+:` / `-:` 两个符号。
- **赋值左侧同样支持位选与部分选择**，包括存储器字内部的位：
  `q[0] <= a[7]; q[7:4] <= a[3:0]; mem[addr][0] <= d;`。提交阶段由“整字写”改成
  “读-改-写”：一条非阻塞赋值只拥有自己那几位，同一拍里写同一个寄存器的多条赋值按书写顺序
  合并，语义与 Verilog 的 NBA 一致（`q[7:4] <= a; q[5] <= 1'b1;` 的 bit 5 取后者）。
- 读取越界下标或跨出值宽度的位返回 X（多位结果里只有越界位是 X，其余位照常驱动）；写目标
  越界或下标未知则整次写入跳过，沿用 RAM 地址越界既有的策略。
- 一条 wire 现在可以由**多条不重叠的 `assign`** 驱动（`assign y[3:0] = a; assign y[7:4] = b;`），
  位段重叠仍按“该位已有连续驱动”拒绝；`signal()` 按位放置每个驱动，没有驱动覆盖的位保持 Z，
  单个整宽驱动的结果与改动前逐位一致。
- 常量折叠（字面量与常量运算符）加入编译器，供 `[msb:lsb]` 边界、`[+:]`/`[-:]` 宽度和复制计数
  在编译期求值；非恒定边界、非恒定复制计数、超过 64 位的拼接/复制都在源码位置报错。
- 内核单元测试新增三组真值断言（拼接/复制/半字节交换、位选与部分选择的读写、`+:`/`-:` 与
  存储器字位选、部分驱动的 wire）与四条拒绝路径断言；
  `build/probe-verilog-grammar.exe` 复核这四类语法从 `REFUSED` 变为 `ACCEPTED`。

## 未发布：运行中元件生命周期修复（2026-10-04）

- **修复"删除元件后再建同名元件，新元件永远不出现在元件栏"**。游戏的
  `custom_prototypes_del` 只把 id 从**有序 live 列表**（`cc_length`/`cc_live_values`，元件栏、
  门数预算与快照三处都读它）里摘掉，原型表（`cc_hashmap`，按 `id & 0xffff` 线性探测）里那条
  仍在；`custom_prototypes_set` 只在探测到空槽时才把 id 追加进 live 列表，于是同一个 id
  重新导入时既被 `add_custom_prototype` 判成重复（-3），也不会再回到元件栏。编辑器文件管理器给
  新文件的 `custom_id` 是**按文件名算的稳定值**（`stableHash`），删掉再建同名文件正好走这条
  路径：真机日志里是 `Component runtime registration rejected: -3`，元件栏里自然什么都没有。
- `tc.component.runtime` 现在记住每个 Mod 自己注销过的 id（`Loaded::retiredIds`）：再次注册这
  些 id 时改走游戏原生的 `update_custom_prototype`（它会重写该 id 的每一份索引），成功后再按
  游戏自己新增条目的写法把 id 补回 live 列表（`{id, id & 0xffff}` 追加到 `cc_live_values`，
  计数 +1，只在 id 确实缺失、计数 < 10000 且目标内存可写时做）；游戏确实已经忘记该 id 时
  退回普通导入。首次注册、替换、注销三条既有路径的行为不变。
- 新增真机回归 `tests/component-runtime-lifecycle-playtest.ps1`（目录条目
  `component-runtime-lifecycle`）：注册 → 注销 → **同一 id 再注册**（断言回到 live 列表）→
  注册第二个 id → 事务式替换，五步全部 `status=0`，并核对真机日志里的
  `put type 0x… back into the component column's live list`。
- 真机复核没有回归：`example.tc-verilog-ram` 的"启动后新建元件"仍把新 custom_id 送进游戏
  菜单树的 `自定义` 分类（`menu-bytes` 探针，`generation 1` 已应用），`tests/tc-verilog-project-playtest.ps1`
  与既有 V3 桥接用例照常通过。

## 未发布：TC-Verilog 可综合内核第一刀（2026-10-03）

- 新增 `sdk/tc_verilog.hpp` / `sdk/tc_verilog.cpp`。源码在 Mod 加载期编译，执行计划由所有
  元件实例共享；寄存器、RAM 与前一拍时钟全部落在 `TCLogicIOV3::state`，可随实例保存和重置。
- 第一批语法包含 ANSI 端口、1～64 位 wire/reg/logic、连续赋值、常用表达式、三目运算、
  `always @(posedge)`、`if/else`、非阻塞赋值和定长 reg 数组。时钟块使用先求值后统一提交的
  NBA 语义，已用 16x8 RAM、计数器和三态加法器回归。
- 内核内部区分 X/Z；V3 无法表示的 X 在边界释放为 Z，未知/越界 RAM 写入不提交。编译失败
  带源码行列且不会替换上一份有效程序，运行时异常则安全释放全部输出。
- 新增 `tc::verilog::Component`：从 module 端口自动生成稳定的 V3 pin 数组，从编译状态布局
  自动填写 `state_words`，并查询 `tc.component.types` V2 完成注册。对象在注册后冻结且不可移动，
  避免宿主持有的回调、字符串或引脚指针失效；假宿主回归覆盖了完整的源码→定义→注册→执行链。
- 新增 TC-Verilog format-1 JSON 项目：严格检查字段和安全相对 `.v` 路径，先读取并编译全部元件，
  成功后才事务式替换项目并批量注册；诊断保留 manifest/source/compile/register/save 阶段、文件、
  元件序号与 ID、行列。`loadAdjacent` 从 DLL 的包缓存目录找默认项目，`bootstrapWorkspace` 首次把它
  落到 Mod 持久目录且不覆盖已有编辑，`writeTextFileAtomic` 为后续游戏内编辑器提供原子保存。
- 新增 `example.tc-verilog-ram.mod`：包内 JSON + `ram16x8.v` + 仅负责加载项目的极小 DLL；首次
  启动生成可编辑工作区，之后每次启动从工作区重新编译并注册同步写、三态读 RAM。
- `tc_ui.h` 正式封装 `igInputTextMultiline`；示例新增主菜单 **Verilog 编辑器**，提供项目文件
  列表、未保存标记、检查语法／保存源码／撤销改动和带文件／行／列的诊断。保存路径由
  `validateAndSaveSource` 钉住为“临时编译成功后才原子替换”，运行中定义保持冻结并提示重启生效。
- 项目新增文件管理器 API：`createSource` / `duplicateSource` / `renameSource` / `removeSource`
  与 `setTypePrefix`。每个操作先校验安全相对路径与源码（新建、复制还要编译通过），再原子改写
  format-1 清单，失败时磁盘上什么都不变；新建与复制分配新的 `custom_id` 与
  `<prefix>/<相对路径>` 形式的 `type_id`，改名只移动文件并保留元件身份，每个项目至少保留一个
  元件。单元测试覆盖成功与拒绝路径，并重新 `load` 清单验证下一次启动仍可载入。
- **Verilog 编辑器** 页新增文件管理工具栏（新建／复制／改名／删除），带名字输入框与删除确认；
  新建与复制写入可编译的缓冲器模板，有未保存改动时管理按钮禁用，操作成功后提示重启生效。
- **同形状热加载**：`Component::hotSwap` 把新编译的程序放进同一个 `Program` 对象（宿主持有的
  回调 `user` 指针因此一直有效），端口与 `state_words` 不变时所有实例从下一个周期起跑新逻辑；
  `Project::saveAndReloadSource` 把“编译 → 原子写盘 → 替换”串成一次保存。形状改变时，这个轻量
  路径仍拒绝直接替换，由下面的 runtime V2 在下一帧重建完整定义。离线用例走宿主的真实回调路径，
  并分别覆盖同形状替换和移动端口后的完整替换。
- **运行中元件完整热更新**：`tc.component.runtime` 已扩展为 V2，在 V1 的 `register_type` 之外新增
  事务式 `replace_type` 与按所有者校验的 `unregister_type`。加载期注册闸门仍只在 `tc_mod_load`
  开放，这个窄例外只在游戏线程、没有电路板时开放；替换失败会恢复旧定义，删除不会碰到其它 Mod
  的类型。`Project::registerLive` / `reloadLive` / `unregisterLive` 与 `Component` 封装覆盖新建、复制、
  改名、端口或状态布局变化及删除，旧加载器继续返回 `TC_COMPONENT_TYPES_ERR_UNAVAILABLE`。
  编辑器在**下一帧**（`on_frame`，不在页面绘制内）提交这些变更，并把结果写进状态栏。
- **可见元件栏同步刷新**：加载器在游戏绘制元件栏之前检查运行时类型 generation；generation 变化时，
  先调用游戏自己的菜单重建函数，再继续原始绘制。这样没有钩住 `reload_component_menu`，不会占用
  Mod/探针需要的同一入口。隔离真机回归中，延迟到启动后创建 `probe_refresh_1400`，日志确认
  `generation 1` 已应用，而游戏菜单探针导出的 `自定义` 分类同时出现新 `custom_id`
  `0xe28f33bd604cd2a9`。
- **修复运行时结构替换被重复 ID 拒绝**：旧实现先从可见原型表删除类型，再调用
  `add_custom_prototype` 导入同一 `custom_id`；游戏还有另一张 ID 索引不会被这个删除同步清空，导致
  新定义和回滚定义都返回重复。替换现在走游戏原生 `update_custom_prototype`，把生成的
  `circuit.data` 从 Mod 私有暂存目录原位更新，再切换逻辑桥和菜单 generation。真机回归在同一进程
  将一个类型从 1 输入更新成 2 输入，并从游戏原型表读回 `2in/1out`。
- **运行中注册新类型的基础契约**：`tc.component.runtime` V1（`register_type`）与
  `sdk/tc_component_runtime.h`。加载期注册闸门只在 `tc_mod_load` 开放，这个窄例外只接受**尚未
  存在**的 `custom_id`，且只在游戏线程、没有电路板时开放，注册后立即 `logic::finish` 激活桥接
  定义，所以下一次进关卡就会被正常编译；`Project::registerLive` / `Component::registerLive`
  封装了它，旧加载器返回 `TC_COMPONENT_TYPES_ERR_UNAVAILABLE` 时调用方回退到"下次启动生效"。
  编辑器的新建/复制在**下一帧**（`on_frame`，不在页面绘制内）提交注册，并把结果写进状态栏；
  真机回归新增断言 `tc-verilog-ram: live component registration available`。
- 交付过程中发现并撤掉了一个副作用：加载器曾钩住游戏的 `reload_component_menu` 来观察菜单，
  这会让 Mod 自己再也钩不上同一个函数（MinHook 返回 `MH_ERROR_ALREADY_CREATED`），现在该钩子
  已完全移除，元件栏相关的既有真机用例（`float-palette-playtest`、`float-menu-probe`）恢复可用。
- 编辑页完成中文化和响应式重排：文件项使用选中态，宽屏左右分栏、窄屏上下排列，尺寸读取
  `igGetContentRegionAvail` 的真实余量；宿主页头改为单行“返回 + 中文页名”，修复高 UI 缩放下
  Mod ID、返回按钮与正文互相覆盖。按钮宽度和底部编译输出空间均由真机帧缓冲截图复核。
- 真机隔离回归已覆盖两次启动：首次注册日志确认 `0x54435652414d3031` 为 5 输入/1 输出，且
  `tc-verilog` 页面完成注册；随后写坏持久 `.v`，第二次启动保留玩家内容并报告
  `ram16x8.v:2:1` 与对应 type ID。

版本号只有一个来源：仓库根目录的 `VERSION`。`build.ps1` 由它生成 `src/version.hpp`，
加载器横幅与日志、安装器标题、`tcmod-cli --version`、分发包文件名全部读这一处
（此前 0.4.0 硬编码在五处，而源码里的界面 API 注释写的是 0.5.0/0.6.0，编号已经对不上）。
当前版本 **0.8.0**（2026-09-27）：0.4.0 之后积攒的全部工作在这一版一次性打包发布。

## 未发布：TCLogicIOV3 三态注册与生成代码桥（2026-09-30）

- `sdk/tc_logic_api.h` 新增 `TCLogicValueV3 { value, drive_mask }` 与
  `TCLogicIOV3`：每个引脚逐位表达 0、1、Z，Z 位和值域外位都规范化为 0；游戏的短路错误 E
  不伪装成第四种逻辑值。
- V3 完整携带 V2 的阶段、实例状态、配置与几何上下文，但引脚元素类型不同，契约明确禁止
  V2/V3 强制转换。公开布局已进入 Windows x64 ABI 快照：值 16 字节、IO 120 字节。
- `tc.component.types` 新增 API V2 与 `register_definition_v3(TCComponentTypeDefinitionV3*)`；
  API V1 保持原尺寸和行为，C++ SDK 新增 `tableV2` / `registerDefinitionV3`。
- 输入数值仍使用已验证的两个 64 位 payload，输入驱动平面在 invoke 前单独锁存；普通网络视为
  全驱动，原版三态网络的 `zid`（刷新态为相邻驱动槽）展开成该脚的全宽 mask。
- V3 生成骨架把每个输出门换成游戏原版 bit/word Switch，并跨 `add_line` 分段改写其使能与
  数据表达式；Z 因而真正不参与网络驱动，0/1 多驱动冲突仍由原版 OR/AND 合并代码报短路。
- 原版 word Switch 只有整字使能，所以当前宽脚只接受 0/全宽两种输出 mask；部分 mask 保守
  发成整脚 Z 并写诊断，不伪造游戏并不存在的逐位高阻。
- 真机 `native-component-playtest.ps1 -Single -V3` 已通过：V3 注册、输入 drive=255、REFRESH
  输出 Z、CYCLE 字节倍增 16 组全部过关，生成源码可见 `drive_in → peek/invoke → drive_out → zid`。
- `build.ps1` 完整通过；Windows x64 ABI 快照增至 1855 条，既有 V1/V2 回归保持通过。

## 未发布：simcore 字宽运算族——原版元件覆盖第一刀（2026-09-29）

沙盒内核要能跑**玩家自己的板子**，而板子一旦离开门级关卡就全是字宽元件。这一刀把
`build/kinds.txt`（该构建自己的原型表，109 个内置原型）里**声明 `wauto` 的二入一出运算**
一次做完，并把"还差哪些、每个缺哪个数"写成清单。

- **新器件族（`simcore/include/tcsim/devices.hpp` 的 `WordOp`）**：`addW`（带进位入/出）、
  `equalW`、`lessU`/`lessS`、`mulW`/`divW`/`modW`、`shlW`/`shrW`/`rolW`/`rorW`/`asrW`、
  `negateW`/`incW`/`notW`/`clzW`/`ctzW`、字宽 `andW`/`orW`/`nandW`/`norW`/`xorW`/`xnorW`，
  以及 2 入 `concat2`；`builtinWordDevice`/`builtinWordDeviceNames` 让调用方先问再建。
- **四态规则照游戏而不是照 Verilog**：游戏生成的程序是**64 位字上的普通 C**，它自己的
  参考仿真器把每个值都解析成具体数，所以字宽运算里**任意一个未定（X/Z）操作数位会把整个
  结果变成 X**（比较类给一个 X）；位级门保留逐位真值。理由写在 `devices.hpp` 该族开头：
  一个游戏不认的"更精确"答案会让交叉验证说谎。
- **板级接入（`simcore/include/tcsim/board.hpp`）**：新增 `wordKindSpec` /
  `wordUnarySpec` / `isConcatKind` 三张 kind→器件表；自动宽度从"每元件一个数"改成
  **每引脚一个数**——移位/循环的**移位量是参数脚**，宽度取到达它的导线，数据脚取整条
  数据通路；权威顺序是①记录 `+0xe0` 的 `memory_bits`、②数据脚上最宽的导线、
  ③原型表声明的宽度。三者都没有时**报"宽度未确定"**，不给 1 位默认器件
  （`0x6a` 这类"输出 `wauto` 但只有配置字段"的元件正落在这里）。
- **离线门槛**：新增 `testWordOps()`（40 条断言：进位入/出、除零、有符号/无符号比较、
  移位量等于字宽的边界、循环绕回、污染规则、Concatenator 高低位、未知名返回空指针）与
  `testWordWidthResolution()`（合成板：Mux 用记录里的 32、Add 退回 8 位导线、**没有任何
  宽度来源的字宽件被报出来而不是建成 1 位**）。
  `simcore` **300/300**、`simcore-view` **188/188**、`simcore-iverilog` **8/8**（门级电路
  的逐事件表与 Icarus Verilog 13.0 仍逐网一致，新代码没有碰到它们）。
- **一处真回归也被门槛抓住**：宽度改成"每引脚一个数"之后，`memory_bits` 会被写进
  **控制脚**（Mux 的选通、Switch 的使能、移位的移位量），把它们撑成 32 位；现在
  `isAutoWidthControlPin` 同时覆盖这三类，记录里的数据宽度永远不写进它们。
- **盘点与清单**：新增 `tools/kind-coverage.py`（读 `build/kinds.txt` 与 `board.hpp` 的表）
  与 [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md)——109 个内置
  原型里 **87 个已能进网表**（16 门级 + 25 字宽/叶子/索引/接口 + 28 手写分支 + 18 空槽），**22 个未支持**，
  并逐个写出缺的是哪一个数（叶子位序、状态件的配置字段、译码器形状、几个探针、
  `0x6a` 的宽度来源）与"用游戏当裁判"的测量方案（`kind-oracle` 探针）。
- **同一轮的第二刀：位序/拼接族**。`board.hpp` 新增 `leafKindSpec` 表，把 14 个
  Splitter / Maker / Concatenator（`0x10/0x11/0x2f/0x30/0x61–0x64/0x6d–0x70/0x73/0x74`）
  接上既有的 `deviceSplitter`/`deviceMaker`；宽度从**原型表自己的引脚形状**取
  （叶子数是几就是几位），拿不到就不建器件并报 `not guessed`。
  `KindPins`/`KindGeometry` 的输出数组从 4 格放宽到 8 格——游戏的八叶 Splitter 之前
  会被几何表整块丢掉。
- **第三刀：Static Indexer（`0x65`）**。`deviceIndexer` 按**游戏自己的两份文件**实现取
  字段：`campaign/symphony_1_nand/test.si` 用 `(instruction >> 24) & 0b1111` 这一组表达式
  解码，同一关的 `hint_custom/Instruction Decoder` 解把七个 indexer 配成**同一组数与
  宽度**（`(29,2) (24,4) (20,4) (16,4) (8,4) (28,1) (0,16)`）；`campaign/multiply` 的解
  用负 index（`-1`…`-7`）左移搭表。于是规则是"index ≥ 0 取 `(A >> index)` 的 width 位，
  index < 0 往另一方向移、低位补 0"，唯一由关卡暗示而非明说的一处（负 index 是否也套
  掩码）写进了注释与待测清单。`BoardComponent` 同时补上 `+0x70` 的 `field_bits` 读数。
- **新增离线测量工具 `tools/netlist-view.py`**：把游戏自己的
  `hint_*.data`/`hint_solution.data`/`hint_custom/**/circuit.data` 读成网表（引脚来自
  `build/kinds.txt`，连线来自文件），`--scan` 还能统计每个 kind 被用了多少次、Splitter
  与 Maker 的叶子怎么对接、indexer 都配了哪些 index。它给出的两条结论：217 份游戏电路里
  **0x51 Output Pin 用了 90 次、0x4f Input Pin 74 次、0x27 Register 71 次**（所以剩下的
  24 个里最该先量的是这三个），以及 §3.2 的位序约定在"关卡自己的解 + 关卡自己的 ISA"
  两处同时成立。
- **第七～九刀：真机测量装置 `kind-oracle`（本轮首次拿到读数）**。新增
  `tests/kind-oracle-probe.cpp`（原生探针：进棋盘 → 用 `tc.commands` 的 `PLACE_COMPONENT`
  放**一个内置 kind** → 用 `tc.board` 把每个元件与导线记录 dump 出来）与
  `tests/kind-oracle-playtest.ps1`（隔离沙盒 + 断言 + 抄走 dump）。跨过去四道真机报出来的坎：
  ①加载器只允许**一个** `igInvisibleButton` 钩子，所以探针复用 `enter-board.cpp` 的加载器、
  只把自己的 `on_frame` 接上去；②`captureBoardObjects` 的 `buffers` 不是可选的（传空指针判
  `ARGUMENT`），而且 `buffers->version` 必须等于 `TC_BOARD_OBJECT_SNAPSHOT_VERSION_1`——正解
  是**带第一版猜测的缓冲调两次**，用第一次失败写回的数量扩容重试；③只能读 `written` 个句柄，
  否则读出来全是 unreadable；④**内置 kind 确实能通过命令总线放上棋盘并读回记录**。
  四份读数（`0x2c` 2 Bit Decoder、`0x27` Register、`0x6a` Input、`0x3e` Switched Input）形状
  一致：`components=2`，index 0 是 `kind=0x00 id=0` 的棋盘自身记录，index 1 是刚放上去的
  kind 并带独立非零实例 id。**这量到的是"能放、能读、能给 id"，行为（值、槽位、状态）还要
  下一步跑周期再读状态数组**；本轮不把"能放"说成"行为已验"。
- **第二十八刀：槽位重排的假设被直接否掉，真机行为测量判为阻塞**。修好缓冲契约后比对编译
  请求前后的导线表：**逐字相同**（`wires=14 w0@268 … w13@264`），所以"重编之后读错了地方"
  这个解释不成立。至此凡是能从这个探针侧检查的前置条件都已排除或直接比对：驱动到位
  （`w11@256` 曾跟着输入走 0,1,2,3）、槽位稳定（本轮）、编译被调用且无报错（§5.18）、命令/
  状态机/模型/符号全正常。**唯一剩下的解释是游戏没有对这块板子执行仿真**——`sim.run` 提交
  成功、`sim.do` 链节也带着有效模型回调了，但没有任何字节写回状态数组；这要从仿真线程/程序
  生成那一侧看，不是帧回调能解决的。**真机行为测量在此判为阻塞**，行为真值表的全部证据仍是
  第 13 轮那一个单点读数。详见
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §5.20。
- **第二十七刀：想直接看槽位表，撞上一条 loader 契约**。为验证"重编把槽位重排了"这个假设，
  加了 `slotMap()`（导线表打成 `id@slot`）并在编译请求前后各打一次。第一次调用就返回
  `unavailable`：这个 helper **只给了 `wires` 缓冲**，而 `capture_objects` 要求
  `buffers->components` 也非空（`native.hpp:508`）。**这是我自己的实现疏漏，不是游戏的问题**；
  槽位重排的假设**尚未验证**。修法很小（照 `wireValues()` 的写法给 helper 也分配 components
  缓冲）。这条线的净结论不变：编译被调用且无报错，但导线字节不随输入变。
- **第二十六刀：帧状态机重写（扁平流水线），前三轮的结论被推翻**。把 `kind-oracle` 的帧回调
  从零重写成"一步一个函数 + 每次转换都记一行"的扁平形式，第一次跑就把真相打全：
  `step wait-for-board -> place-target` → `placement state=3 result=0` →
  `want-compile model=…2650 preorder=…1a20` → **`asked the game to recompile the board`**
  （`preorder progress empty`，不是错误）→ `run-once` → `sweep-write/wait`。
  因此**第 23～25 轮的三条结论全部更正**：`preorder` **被调用了**、指针**不是 null**、
  `sim_model` 那一刻**非空**。真正没解决的仍是同一件事：编译请求发出且无报错，但扫描里
  `w11@256` 四行都是 0（之前能跟着输入走）。下一处要查：这次重编是不是把棋盘状态清掉了，
  或 `sim.run` 有没有真的跑起来（`sim.do` 链节日志可直接回答）。
  **方法论教训**（已写进研究文档）：连续三轮我在"日志的缺失"上做推断，而真正的错误是
  **日志本身没被执行**；遇到"某件事没发生"，第一步应先确认那行日志有没有执行。
- **第二十五刀：`set_sim_test` 也接上了，诊断范围压到一个日志语句**。按 §5.16 抄了
  `component-capacity-probe` 的上下文：`set_sim_test__modelZutilities_u6840` 解析成功
  （`set_sim_test=…cea0`），`recompile()` 里先调它再调 `preorder`；为了定位又在"该编译了"的
  决策点加了一行日志（打印 `sim_model` 与 `placed_frame`）。**结果这一行也没出现**——所以
  根因不在 `preorder`，而在决策点之前：`placed_frame >= 0` 在帧回调里（我放这段的位置）
  没有成立，或那段代码没被执行到。**下一步只看这一行为什么没输出**。本轮到此不再改探针。
- **第二十四刀：符号是好的，编译调用仍未落地——这条线判为阻塞**。那一行日志跑掉了：
  `preorder symbol=…1a20 fn=…1a20`，**解析完全正常**。所以唯一剩下的原因是 `sim_model` 在
  `recompile()` 那一刻还是 null（模型只在 `sim.do` 响应过一次运行请求后才出现），据此把条件
  改成"模型到手再请编译"并重跑，**`asked the game`/`preorder progress` 依然一次都没出现**，
  读数逐字未变。至此**四件前置条件（符号、模型、命令、状态机）全部用真机日志排除**，这条线
  判为阻塞：探针无法让游戏在程序化放置之后重算棋盘，行为真值表因此拿不到。
  给接手人的最短路径写在 [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md)
  §5.16：**照 `component-capacity-probe.cpp` 的上下文抄**——它是在 `set_sim_test(model,0,1)`
  之后才 `preorder(model,&progress,0)`，游戏可能要求先进入测试态才肯编译。
- **第二十三刀：状态机修好了，`preorder` 一次都没被调用**。三值日志打出真相：
  `trace stage=1 request=1 placed_frame=-1` → `placed_frame=1..5` → `stage=2`。**第 22 轮那个
  bug 修对了**——放置命令以 SUCCEEDED 结束、`placed_frame` 开始累加、状态机从 1 走到 2，
  命令总线那一环好用。**但 `recompile()` 仍没生效**：日志里 `preorder` 出现 0 次。它的前置
  条件是 `!preorder_fn || !sim_model`，而 `sim_model` 已被证明是好的
  （`sim.do link fired … model=…24e0`），所以只剩一种解释：**`preorder_fn` 解析出来是 null**，
  函数在第一行就返回了。下一轮唯一要看的：把该符号的解析结果打进日志（一行）；为 null 就在
  符号画像里换名字，不为 null 就是调用时机（同一帧里先 recompile 再 run，`sim_model` 那一刻
  还是 null）。
- **第二十二刀：找到并修掉一个真 bug，状态行仍未打出**。放置命令的状态检查原先**写在
  `kWaitingForPlacement` 分支里面**，而状态机发出 run 请求后就转到 `kRunning`——那个分支再也
  不执行，于是 `placed_frame` 永远是 -1、`recompile()` 的条件永远不成立。这解释了第 21 轮
  "模型是好的、编译却没跑"。修法是把状态检查提到 stage 门之前（每帧一次），代码已编译通过。
  **但真机跑完 `placement state=…` 这一行仍没出现**，读数逐字未变，说明
  `placed_frame < 0 && request != 0` 这个条件本身没成立（要么帧里看到的 `request` 是 0，
  而提交那一帧明确写着 `request=1`；要么那段没被执行到）。下一轮唯一要看的地方：在帧回调最
  开头无条件打 `request`/`stage`/`placed_frame` 三个值。
- **第二十一刀：假设被推翻，问题缩到"放置命令的状态"**。上一轮猜"模型指针拿不到"是**错的**：
  `sim.do` 链节真机打出了有效指针（`args=…0f90 model=…2770`，多次稳定）。既然模型是好的、
  日志里却依然没有任何 `preorder`/`recompile` 输出，说明 `recompile()` 的调用点没走到；而它
  的条件是 `extra_index >= extra_placements.size() && placed_frame >= 0`，所以唯一的解释是
  **`placed_frame` 仍是 -1**——"放置命令以 SUCCEEDED 结束"这一支从未成立。下一轮只需把
  `TCCommandStatusV1` 的 `state/result/completed_frame` 每次都打进日志，就能定位是命令没成功
  还是状态读取分支写错了。
  **`kind-oracle` 到此确认可用的一半**：进关卡、放内置 kind（可多个）、读回元件/导线记录、
  按棋盘下标驱动输入、读多字节槽位、`sim.do` 链节拿到模型指针。**未确认的一半**：程序化
  放置之后让游戏重算这块板子——行为真值表依赖它。详见
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §5.12。
- **第二十刀：把第 19 轮的变形回退掉，并把"为什么没编译"缩到一行**。去掉"每个值重编一次"
  之后，读数**恢复到 §5.6 那个形状**（`w11@256` 又跟着输入走 0,1），所以那次变形确实是多余
  重编造成的。同时查明 `recompile()` **一次都没被调用**：两个钩子都挂上、`preorder` 也解析
  到了，但日志里没有任何 `preorder progress` 输出；代码路径核对过（钩子安装与 `preorder_fn`
  赋值都在 `enter_board_load` 之后、`create_hook` 没有被清空），所以最可能是
  **`tc::hook::simDoArgs(call)->model` 没给出模型指针**、`sim_model` 始终为 null。下一轮第一件
  事就一行：把 `args->model` 的原始值打进日志确认。`recompile()` 现在也会把 `preorder` 交回的
  progress 字符串写进日志，好让"编译到底有没有发生"一眼可见。
- **第十九刀：真去接了编译入口——三条都挂上，行为读数仍然没出来（失败实验）**。
  `handle_update_wire` 钩子（取模型指针）与 `sim.do` 链都**真机确认挂上**
  （`model capture armed`、`Hook chain sim.do joined (priority 0)`），
  `preorder__modelZsimulationZcompile95thread_u3523` 也解析到了。但模型指针要等游戏处理过
  一次运行请求才出现，于是 `recompile()` 在第一次扫描前一直拿不到模型；改成"每帧重试 +
  每个值重编一次"后读数反而变形：`w11@256` 不再跟着输入走（四行都是 0），`w2@264` 仍是 1。
  **这一轮既没拿到真值表，也没保住上一轮能用的形状**——按失败记录，不写成进展。
  下一步只做一件事：把 `preorder` 调用的返回值与游戏侧副作用记清楚，再决定是编译时机不对
  还是这条路不对；弄清之前不动 `kind-oracle` 主流程。
- **第十八刀：把"缺的那一环"写成可执行的接法（未实现）**。查明 `preorder__modelZsimulation
  Zcompile95thread_u3523` 就是"让游戏重新编译这块板子"的入口（`component-capacity-probe`
  在放完元件后正是靠它把编译跑起来，两行代码），**缺的只是 `model` 指针**：那个探针是从
  `handle_update_wire` 的钩子第一个参数取的，而 `kind-oracle` 因为钩子位被 `enter-board`
  占用（§5.1 第 1 条）一个钩子都没挂。两条可行路线（再挂一个 `handle_update_wire`，或查服务
  表里有没有直接交出模型的入口）写在
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §5.9。本轮只做了
  调查与记录，**没有实现**，也没有改口 §5.8 的结论。
- **第十七刀：完整关卡契约也部署了，结论不变——这条线在此收手**。试玩脚本新增
  `-LevelSource`，把 `campaign/overture_decoder` 的 `meta.txt`/`test.si`/`ui.txt`/`label.txt`
  连同我们的 `circuit.data` 一起部署（已确认写出 `Copied … into campaign\sandbox`），读数
  **与之前逐字相同**；再把等待从 60 帧加到 300 帧、每个值从 30 帧加到 120 帧，也一样。
  四种组合（只放电路 / 电路+常量驱动 / 电路+完整关卡契约 / 再加长等待）给出同一种形状，
  而 `sim.run` 与 `write_input` 都返回成功——所以**这块板子的线状态字节从头到尾没有被重算
  过**，它们是关卡加载时的初值。要拿到行为读数，需要在放置元件之后**让游戏重新编译这块
  板子**（`compile_request__modelZutilities_u…`，用法见 `component-placement-probe.cpp`），
  那是 `kind-oracle` 之外的一件独立工作。
  **本线到此收手**：装置里已真机验证的部分保留（放内置 kind、多放常量、读回元件/导线记录、
  按棋盘下标驱动输入、按字节含多字节读槽位），"让游戏重算"这一环缺着，行为真值表仍只有
  第十三刀那个单点读数（地址 0 → `out0=1`）。详见
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §5.8。
- **第十六刀：查明"值不传播"不是驱动的问题，是这块板子在沙盒里根本没被重算**。
  上一轮四次扫描里关卡输入确实写进了棋盘（8 位那根线读出 0,1,2,3），但下游的线**一个字节
  都没动**。这一轮换掉驱动方式再测一次：探针新增 `TC_KIND_ORACLE_EXTRA`（`kind@x,y;…`，
  在放目标之前先放别的元件），真机确认**放两个 ON 常量（`0x02`）到棋盘上可行**
  （`component index=1 kind=0x02 x=4 y=0`、`index=2 kind=0x02 x=4 y=2`，各带独立实例 id）。
  但读数形状与上一轮一样：绝大多数导线的字节是 0。
  **结论（本轮确定的这一条）**：沙盒里那块板子在关卡没有自己的 `test.si`、也没有声明 I/O 时，
  游戏**不会重算**它的线——所以"关卡 Input 的驱动"和"常量驱动"两条路都量不到行为。
  要拿到真值表，得让关卡**有 I/O 与测试**（把 `campaign/overture_decoder` 那类的
  `meta.txt`/`test.si` 一并部署，或改成用游戏自带的 `byte_mux` 那种已验关卡当宿主）。
  这条不是猜测：两次不同驱动的读数形状一致，且 `sim.run`、`write_input` 都返回成功。
- **第十五刀：扫描跑起来了（译码器仍未跟着输入变）**。修掉两个自己的错之后四次扫描跑通：
  ①扫描必须隐含"跑仿真"那段——第一版把扫描分支挂在 run 阶段之后，而那次没给 `-Run`，状态机
  从没进过扫描就直接 dump 了；②驱动要写的是**棋盘上的元件下标**（关卡自己的 Input 是
  **index 4**，不是电路文件里的 8），写对之后能确认驱动生效——8 位那根导线在四次里读出
  **0,1,2,3**。但同一份读数里**译码器的输入线始终 0、输出线始终 1**，也就是说
  Splitter→译码器那段没跟着输入动，两种解释（关卡输入在沙盒里没有 `test.si` 驱动 /
  我对"哪根线是译码器输入"的判定有误）本轮**没有定论**。装置本身（逐值扫描、多字节槽位
  读取）已在真机上验证；**译码器的真值表仍未拿到**。下一轮候选：确认 2 位线与 `w11` 是否
  同网，或改用探针放常量元件当驱动源以绕开 `input_replay`。
- **第十四刀：真值表扫描的装置（装好了，但这一轮没跑起来）**。`tc.io_value` 提供按  **棋盘元件下标**读写的 `write_input`/`read_input`/`flip_input`，所以扫描不必改关卡文件；
  探针新增 `TC_KIND_ORACLE_SWEEP_INPUT/FROM/TO`（逐值 `write_input` → 让游戏跑一会儿 →
  把每条导线记成 `id@slot=value` 一行），试玩脚本新增 `-SweepInput/-SweepFrom/-SweepTo`。
  **但实跑出来的是普通 dump 而不是扫描**：loader 日志里 `sim.run` 与 `sweep input` 两行
  都没出现，说明探针没进那两个分支（最可能是环境变量没被读到）。装置**尚未被真机验证**，
  本轮不声称可用；下一轮第一件事是打印探针读到的参数值、把参数通路确认掉。
- **第十三刀：第一份行为层面的真机读数**。装置打通了整条链：`tools/level-circuit.py` 生成
  关卡电路 → 试玩脚本把它部署进隔离沙盒的 `campaign/<关卡>/circuit.data`（新增
  `-CircuitPath`）→ 探针读游戏自己的 `simulation_state` 缓冲区（新增
  `TC_KIND_ORACLE_RUN`，先让游戏跑 N 个周期）→ 每条导线记录带上**当时的字节值**。
  结果：游戏加载并运行了我们部署的电路（`components=10 wires=14`，即那份解的七个元件加上
  探针放上去的第九个），状态偏移以 2 字节步长分配且**线间共享**（`268` 被三根线共用）。
  可对照的一条：译码器 `out0` 那根线（`slot=264`）的值是 **1**，而关卡 Input 初值为 0——
  与 `deviceDecoder(2,false)`"地址 0 只点亮 out0"一致。**这是一份单点读数，不是真值表**；
  地址 1/2/3 要下一轮改初值或找驱动路径再量，本轮不把它说成"语义已验"。
- **第十二刀：关卡电路生成器落地**。新增 `tools/level-circuit.py`：以游戏自己的解为模板
  生成"能被部署、能被解析器读回"的关卡电路。**一条实测教训**：第一版想"只保留某些 kind、
  删掉多余导线"，结果 14 根导线全被丢光——那份解把多根导线接在同一格上（总线式共享），
  按"两端都落在保留引脚格上"过滤不成立。现在的做法是**不改电路形状**，只（可选地）给
  关卡自己的 Input 改名/改宽度再编码写回。实测：把 `overture_decoder` 的解改成
  `--rename-input Instruction` 写进 `build/decoder-level/`，再读回来是
  `components 8 wires 14 version 13`、Input 名字为 `'Instruction'`，并已带上 `sandbox` 的
  `meta.txt`/`label.txt`。
- **第十一刀：行为测量的落脚点探明**。①**编码器可用**：`tools/circuit_format.py` 的
  v13/14 编解码往返通过（读游戏电路 → 编码 → 解码，元件与导线数量一致），`python-snappy`
  本机可用，所以"写自己的电路文件"没有工具障碍。②**模板找到了**：
  `campaign/overture_decoder/hint_solution.data` 本身就是一份最小译码器电路
  （`0x6a Input` → `0x11 Splitter` → `0x2c 2 Bit Decoder` → 四个 `0x3a Output` + `0x6f Maker`），
  拿它当模板就不用从零构造元件记录。③**新增 `tools/pin-slots.py`**：打印"哪根导线落在哪个
  元件的哪个脚上"，它在这份解上验证了自己——四个输出脚各引出一根线到四个独立 Output，
  输入侧 `in0` 接 Splitter、Splitter 吃 Input，**这就是一位热码译码器在真机电路里的形状**。
  ④**探明一条限制**：关卡电路里**没有**状态偏移，偏移是游戏加载时分配的（所以探针要 dump
  导线记录，每条都带 `state_byte_offset`）。
- **第十刀：把"行为那一半"的路线定下来**。带坐标与旋转的读数（`-X 7 -Y -5 -Rotation 1`
  读回 `x=7 y=-5 rotation=1`）确认装置可以按坐标/旋转量记录，探针也在放置后多等 5 帧让
  游戏安定记录。行为测量真正难的是**引脚在棋盘上的位置**（`0x37` 这一类在原型表里没有
  声明），三条路按风险排序写进了
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §5.1：①写状态数组
  （需槽位号）；②替游戏写导线记录（布局已实测，但属于直接改游戏内存）；③**用关卡内容当
  模板**——把目标 kind 与常量/输出脚写成一份 v14 电路、部署成关卡让游戏自己加载运行
  （`tests/level-content-playtest.ps1` 已证明这条路可行，编码器在 `tools/circuit_format.py`）。
  按红线 6 的取向**优先第 3 条**：它量到的是"游戏运行我们写的电路"的真机行为，且完全不
  碰游戏内存。
- **第六刀不是实现，而是把两种"缺"分开**：原型表里有 **22 个 kind 完全没有声明任何引脚**，
  但它们的原因不同，下一步该往哪查也因此不同——
  ①**状态件**（`0x76` RAM、`0x52`/`0x53` Memory Probe）：端口由板级接入自己建模（RAM 的
  Load/Store 插座顺序已经在 `board.hpp` 里），值在别处，不是"没量到引脚"；
  ②**真的缺几何**（`0x37` Word Delay Line、`0x0d` Bit Delay Line）：这两件在游戏电路里
  `out0` 100% 接线（11/11），而原型表那一行没有任何 `in…`/`out…` 声明——**输入端的位置
  只有游戏自己知道**。它们的记录指纹已经量到（`bits=32` / `bits=1`），所以缺的**只是端口
  几何**，要么真机把引脚位置读出来，要么先只支持输出端这一半。这是本线第一条"缺的不是
  字段值、而是引脚位置"的项目，写进 [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §4。
  **当轮又量到一半**：把 217 份电路里所有 Delay Line 的坐标与导线端点对照，落点**恰好只有
  两个**——`(-3,0)` 与 `(3,0)`，各 11/11 命中（`(3,0)` 就是表里没写出来的那个 `out0`）。
  于是几何其实已经量出来了，**没量到的是行为**（那根输入是不是"下一拍出现在输出"），
  按红线 6 在那之前不建器件。
- **第五刀：译码器三件（`0x2b` / `0x2c` / `0x2d`）**。`deviceDecoder` 按原型表自己声明的
  宽度实现一位热码译码：`0x2b` 一个地址位两位出、`0x2c` 两位地址四位出、`0x2d` 三位地址
  加**一个使能脚**、八位出；未定的地址或使能一律给 X，使能为低时全部输出 0，而**使能不接
  线时保持使能**（这一处是推断而非实测：它是唯一一个声明偏移不在数据通路上的脚
  `(0,-4)`，在游戏电路的 11 次出现里只接了 1 次，且那一次承载的是一位信号——已写进
  待测清单）。行为形状的证据来自游戏自己的 ALU 解：译码器的 `out0..out2` 接进一个
  4 输入 Maker 的**高三个叶子**、Splitter 的一位接进最低叶子，也就是一个四选一的选择字。
- **第四刀：接口引脚（`0x4f` Input Pin / `0x51` Output Pin）**。实现为**宽度可变的直通**
  （`devicePin`，没有驱动时输出 Z）。证据是结构性的、覆盖全部 217 份游戏电路：
  `--scan` 显示**每一份** Output Pin 的 `in0` 都接着线、`out0` 都不接（90/90），
  **每一份** Input Pin 反过来（74/74）——这正是直通件的形状，源件与汇件都对不上。
  这两个 kind 在游戏电路里的用量（90 与 74）是剩下那些里最高的两个。
- **剩下 22 个的用法与记录指纹全部量出来了**（`--scan` 新增两项：每个 kind 的每根脚
  "在多少次出现里真的接着线"，以及它的记录在文件格式暴露的字段上长什么样）。表在
  [research/simcore-kind-coverage.md](research/simcore-kind-coverage.md) §4，读出来三件事：
  ①**十二个 kind 在游戏自己的电路里一次都没出现**（Auto Register / Auto Delay Line /
  7 Segment / Static Value / Static Evaluator / Auto Static Indexer / Halt /
  Disassembling probe / Level Component / Level And Delay / 两个无名槽），优先级最低；
  ②**`0x27` Register 的入口不在棋盘上**——71 次出现里 `in0` 一次都没接、`out0` 只接 33 次，
  记录里 `i16` 是一串负数而 `boolA=1`，而 `campaign/assembly_programming` 里它们就叫
  `Reg 0`…`Reg 5`（同电路还有 `Count`、`Program`），说明它是靠名字/索引与仿真器打交道的
  寄存器堆，**必须真机读数**；③`0x3e` Switched Input 的 `in0` 只有 1/20 接线，属于
  "值由元件自己配置决定"的源件那一类。
- **一处两可之间的选择被写明**：原型表给了叶子的偏移与宽度，但没说最上面那片叶子是
  最高位还是最低位；本版取"最上面的叶子是最高位"，理由是它让 Splitter 与 Maker 互为
  精确逆运算（`bus_split` 已与 iverilog 逐网对过）、与 2 入 Concatenator 由声明宽度
  强制的读法一致，并且（第三刀之后）是唯一能让 `symphony_1_nand` 的解与它自己的 ISA
  对上的读法。这是**命名**层面的选择而非发明行为，真机那一次对照仍列在待测清单里。
- **本轮的三个真 bug 都是离线门槛抓的**：`switch` 的组合 case 标签被编辑挤掉、字宽按位
  运算读 `BitVector` 时把位序读反（MSB-first 向量与 C 值的位序）、以及"先污染后比较"
  让比较件输出 4 位而不是 1 位。


当前版本 **0.8.1**（2026-09-28）：在 0.8.0 之上把 Float Ops 铺到 binary16/binary32/binary64
三个宽度（含加载器新的宽度感知十进制浮点字段）、Mod 可替换关卡内容，并修了底部元件属性栏的
一轮排版与交互问题。

## 未发布：沙盒兼容自定义元件（kind 0x4E）（2026-09-30）

### 补：定义内部的导线一直在被读错（2026-09-30）

顺着玩家板子上剩下的 6 条 `component pins 2 share the cell …` 量下去，翻出三个真问题——
它们都不在那 4 条 note 本身，而在**展平自己的数据来源**上：

- **导线步长量错了**。文档与代码都写着"定义里的导线元素步长 `0x40`"（探针当时的猜测），
  真机内存里腿指针（`+0x28`）是每 **`0x68`** 一次、端点每 `0x68` 落在 `+0x18`——和棋盘
  导线记录**同一套布局**。用 0x40 读，每隔一条记录就落在前一条中间，7 份 foundry 定义的
  内部导线大半读成 `0,0 -> 0,0`。改回 0x68 后**没有一条**退化，玩家那块板子的网表从
  130 网/104 器件 变成 **183 网/146 器件**：以前少掉的内部连接根本没接上。
- **定义导线不带宽度**（248/248 实测为 0，棋盘导线写在 `+0x30`）。所以网宽改为"导线自己的
  宽度优先，导线不带宽度时取落在它上面的引脚宽度（引脚声明，或 `wauto` 时该元件记录的
  `+0xe0`）"——否则自定义元件内部每条 32 位通路都会被建成 1 位网络。
- **嵌套定义被叠在父定义的坐标系里**。一份定义里可以再放一颗自定义元件，而它的内部件通常
  比实例占的格子大得多，于是盖住父定义其它元件、凭空造出短接——那 6 条 note 全是这么来的
  （含一条"四分线器 `out3` 压在**另一份定义**取负器 `out0` 上"）。现在每个实例（含嵌套）
  分到自己那块 200 格区域；区域数超出 int16 容量时直接拒绝展平，不再冒重叠的险。
- **顺带**：那 6 条 note 现在会**说出是哪两个引脚**（`component 65 (kind 0x6E) out3 at …`），
  不再只报一个格子坐标——这次就是靠它定位到"两份不同定义"的。
- **验证**：`simcore` **372/372**（新增"宽度为 0 的导线按引脚取宽"两条断言）、
  `view-tests` **197/197**；真机按玩家完整配置连跑**三轮**：**0 崩溃、0 note**、
  每轮都是 183 网 / 146 器件，查看器读同一份记录得到同样的 183 网。

### 补：两族 `wauto` 的宽度量到了，Word Maker 不再被拒（2026-09-30）

上一轮之后，玩家板子上还剩一条清不掉的 note：`component 155 (maker2) has no width its
prototype pins down; not guessed`。原型表把"自动宽度分线器"（`0x2f`/`0x63`/`0x64`）和
Word Maker / 宽 Concatenator（`0x30`/`0x61`/`0x62`/`0x73`/`0x74`）的数据通路写成哨兵，
游戏自带的 217 份电路里这两族**一次都没出现**，所以只能量真板子。

- **新工具 `tools/kind-wire-widths.py`**：把 `sandbox-sim-board.txt` 里每个实例的引脚
  （偏移来自 `build/kinds.txt`，含旋转）与**接到它上面的导线的宽度**对出来。玩家板上两条
  读数：`0x2F in0 64 -> out0 32, out1 32`、`0x30 in0 8, in1 8 -> out0 16`；记录自己的
  `+0xe0` 独立印证（32 = 每片宽度、16 = 输出总宽）。
- **规则**：Splitter 的输入 = 叶子数 × n（n 取输出线上量到的宽度、或输入 ÷ 叶子数、或记录
  的 `+0xe0`）；Word Maker / 宽 Concatenator 的每个输入保留**各自导线**的宽度，输出是它们
  的拼接（最上面的输入最高位，沿用 §3.2 的位序约定）。
- **器件**：`deviceWordSplitter(leaves, n)`（2/4/8 出）与 `deviceWordMaker(widths)`
  （N 入、逐入宽度、输出为和）；位 Maker（`0x10`/`0x6f`/`0x70`）成为"每个叶子 1 位"的特例。
  顺带修掉两处旧账：`0x2f` 的 n 以前会去读**不存在的输入脚**并把记录的宽度再除 2；输出脚
  的线宽度以前会混进输入宽度表（Word Maker 因此会被算成 16+8）。
- **门槛**：`simcore` **370/370**（新增 `testAutoWidthSplitterAndMaker()`：器件真值表
  `10101011`+`11001101 → 1010101111001101`、四路分线 `16 → 4×4`，以及两块合成板钉住
  "宽度从导线来"的解析顺序）、`view-tests` **197/197**。
- **真机**：玩家那块板子从"一件被拒"变成全部建成（`u156_wordmaker`、`u152_wordsplit2x32`），
  report 里只剩 4 条 `component pins 2 share the cell …` 的几何 note，窗口读同一份记录得到
  一样的 130 nets / 235 evaluations 与一样的 note。

### 修：存储家族的 cycle 哨兵网络让 `publish()` 越界（2026-09-30）

打开沙盒时游戏**偶发崩溃**，加载器的故障日志写着
`dev.sandbox-sim in on_frame: access violation at 0x0000000000000000`。

- **根因**：`board.hpp` 为存储家族（寄存器/计数器/延迟线）新增的 `#cycle` 哨兵网络
  用 `addNet` 建出来，却**没有同步扩容 `slots_of_net`**；而 `SandboxRuntime::publish()`
  是**按网络下标**遍历 `slots_of_net[net]` 的。少了最后一个条目，它读到的是分配器留在
  那里的一个 `std::vector`——内容碰巧是空指针时就是一次空指针解引用，所以表现成
  "偶发"，而且只有带寄存器的板子才会（8/29 的板子没有 cycle token 时不会崩）。
- **修**：`board.hpp` 建 `#cycle` 之后立刻 `slots_of_net.resize(netCount())`；
  `runtime.hpp` 的 `publish()` 再加一条边界保护，让"某个构建器忘了给网络配槽位表"
  永远只是少写几个字节，而不是崩溃。
- **回归测试**：`testStorageFamily()` 新增三条断言——每个网络都有槽位表（含 cycle token）、
  该板子能绑上 runtime、`publish()` 能走完所有网络。把修复撤掉时这条会 **FAIL**
  （`FAIL every net has a slot list, the cycle token included`），修复在时
  `simcore` **361/361**、`view-tests` **197/197**。
- **真机验证**：`tests/sandbox-custom-playtest.ps1` 现在可以按玩家的完整配置跑
  （新增 `-Dump` / `-Mods` / `-Config` 三个参数，这次的复现就是它们做的）：
  `-Mods dev.sandbox-sim,dev.enter-board,local.float-ops -Config 'vcd=1;vcd_cycles=40;view=1;startup_seed=1;free_run=1;trace=1'`
  在修之前 **3 轮里 2 轮**写出 `fault.log`，修之后**连跑 3 轮 0 条**，
  并且三轮都真的在跑（`cycles_run=143…176`、`published≈1.1e5`）。

### 补：独立查看器不再拒跑这些板子（2026-09-30）

沙盒在游戏里跑得好好的，独立窗口 `simcore-view` 却对同一块板子报
`component N (kind 0x2D) … no pin geometry in the frozen table`、
`component N (kind 0x2F) has no device in the library`——玩家点名的**分线器、
3-8 译码器、多位宽寄存器**正是这三条。三处原因，逐条修掉：

- **窗口里的几何表是手抄的，抄落后了。** 真机版从游戏的实时原型表取几何，窗口没有游戏，
  只能读 `simcore/view/kind_pins.hpp` 里那份冻结副本，而那份副本停在第一次写的时候：
  `0x2D`/`0x27`/`0x10` 没有引脚、`0x2C`/`0x30`/`0x2F` 没有器件分支，整块板因此拒跑。
  现在这份表由 **`tools/kind-pins.py` 从 `build/kinds.txt` 生成**
  （`simcore/view/kind_pins_generated.hpp`，`--check` 可查是否过期），并且每行的宽度按
  `prototypeGeometry` 的读法填（"auto" 哨兵与 `w0` 都是 1，两边的判断因此逐条一致——
  `wauto` 的自动宽度 Splitter/Word Maker 在两边一样被明确拒绝，而不是一边猜一个宽度）。
- **记录里本来就带着引脚，窗口却只看冻结表。** 实时版现在为**每一个**元件写
  `inbits=/outbits=/inpins=/outpins=`（Mod 类型与内置元件一视同仁，顺序是原型表的
  声明脚再隐藏脚）。窗口把它们按实例 id 注册，优先级高于冻结表；冻结表退化成"没有这些
  字段的旧夹具"的兜底。引脚上限同时从 8/4 提到 `KindPins::kMaxPins`（64），否则
  8 出的 3 Bit Decoder 只画得出 4 根。
- **自定义元件在窗口里没有定义可展平。** 记录现在额外写出 `defcustom`/`defcomponent`/
  `defwire` 段（含嵌套定义），展平算法从 `plugin.cpp` 提到共享头
  `simcore/include/tcsim/custom.hpp`，窗口和真机跑同一份代码。**实例保留自己的槽位**
  （改成 kind 0，builder 会跳过），内部件追加在板子自己的组件之后——这样"按棋盘下标"
  的绘制与器件表仍然指同一个元件，而网表是展平后的那份。
- **顺带**：`+0x70`（Static Indexer 的字段宽度）现在随记录走，否则窗口只能报
  "declares a 0-bit field"；`field_bits=` 由 `board_file.hpp` 读回。
- **验证**：离线 `view-tests` **197/197**（新增 `testCustomComponentFlattens`，夹具
  `simcore/view/data/custom_passthrough.txt`：定义是 Input Pin 直连 Output Pin，断言常量的
  1 真的从实例输出脚出来）；`simcore-tests` 358/358。真机端到端：`tests/sandbox-custom-playtest.ps1 -Dump`
  导出玩家那块 52 元件、7 份 foundry 定义的板子，窗口读它得到 **130 nets / 233 evaluations**，
  不再拒跑，报的 note 与游戏内那份 report 一字不差（见下条）。
- **仍未做（两边一致地报出来，不假装）**：那块板子上有一个 `maker2`（Word Maker）
  的宽度只写了 `wauto`，原型表没有能落地的数，两边都拒绝它并各报一条 note；
  另有 4 处"两个引脚同格"的几何告警（`component pins 2 share the cell …`）。

- **自定义元件不再是"未支持"。** 在游戏里它们不是器件，而是一份**定义**：游戏把一份电路
  解析后放进自定义原型表。沙盒现在把这份定义**展平**进网表——读自定义原型里的组件数组
  （`+0xe0`/`+0xe8`）与导线数组（`+0x100`/`+0x108`），把内部元件与导线整体搬到远端坐标
  （绝不与棋盘撞格），再按**序号**把定义边界脚（`0x4f` Input Pin / `0x51` Output Pin）
  接到实例自己的引脚上。棋盘原有的外线停在实例脚格上，桥接线共享端点即成同一网络，
  所以外层接线一个字都不用改。测量与设计写在
  [research/custom-components.md](research/custom-components.md)。
- **几何上限**：`KindPins` 每侧从 8 个脚提到 **64**。原来的 8/4 是照内置元件定的，
  而自定义元件的接口是作者画的——玩家的 `Symphony/DEC` 就是 1 进 7 出，之前因此得到
  `component 7 has kind 0x4E with no pin geometry`。
- **真机结果**：玩家那块带 5 种自制元件（`Symphony/DEC`、`ALU`、`cond`、`count`、
  `ADDRDECODER-3`）的 CPU 板，从"整块拒跑"变成 **nets=130 / devices=103** 的正常网表；
  新用例 `sandbox-custom`（`tests/sandbox-custom-playtest.ps1`）把它做成回归。顺带新增
  `tools/definition-layout.py`（复核定义字段偏移）与 `tests/custom-prototype-probe.cpp`
  （只读 dump 自定义原型与定义数组）。
- **细节**：定义里的组件记录不重复引脚数（`+0x30`/`+0x40` 读出 0），沙盒改从原型表取，
  与几何回调同一依据；内部网没有游戏的状态字节，因此用新的哨兵 `tcsim::kNoStateSlot`
  明确"不要写回"，不再有内部线写进槽 0 的风险；定义结构里的两个 Splitter/Maker 宽度哨兵
  （`0x7ffe` 一类）不再被当成真宽度（`kind_bit_width` 收紧了判据）。
- **仍未做**：Mod 注册的**原生**元件（`local.float-ops` 之类）的"定义"是 C++ 回调而不是
  电路，展平对它们不适用；让它们跑起来需要"宿主以给定输入调用该 Mod 回调"的入口。展平后的
  **数值**与游戏逐周期对照也还没做（本轮证明的是能建表、能跑）。见
  [research/custom-components.md](research/custom-components.md) §6。

## 未发布：仿真器补齐最后的原版元件——存储家族与关卡件（2026-09-30）

- **沙盒仿真器现在认识全部 109 个内置原型**（只剩 `0x50` 一个：游戏自己拒绝放置它，
  交付形态是"识别并明确报未支持"）。上一轮结束后还缺 19 个，这一轮把三组测量一次做完，
  依据不是黑盒扫输入输出，而是**游戏自己编译出来的程序**：新工具
  `tests/kind-source-dump.ps1` 跑一块带该元件的板子（`TC_MODLOADER_DUMP_SOURCE=1`），
  把游戏生成的 `native-logic-source-*.txt` 收进 `build/kind-source-<tag>/`，那份代码就是契约。
- **引脚**：存储件的输入脚在原型表的**第二个**列表里（`+0x70`/`+0x78`），
  `tests/kind-list-probe.cpp` 现在也把它 dump 出来（`hidden0=(-3,-1,w1)` 之类）：
  寄存器 = enable(-3,-1) + data(-3,0)，计数器 = reset(-3,-1) + data(-3,0)，
  延迟线 = data(-3,0)；Auto 版多一个 (0,-2) 的"蓝脚"（宽度来源）。记录算的是**总数**，
  真机原话：`component 3 (Counter) has 2/1 pins in its record but the prototype has 0/1`——
  `KindPins` 因此分成 `input_count`（总数，与记录对齐）与 `declared_inputs`。
- **行为**：寄存器 `enable ? data : 保持`、延迟线 `每周期 = 输入`、计数器
  `reset ? 0 : value+1`，三者的锁存都发生在游戏的 LATE 阶段（`cycle += 1` 之前）。
  simcore 为此第一次有了**每实例存储**（`DeviceSpec::state_behavior` + `state_size`），
  板级一条私有网 `cycle_token` 由运行时不漏地每个周期边界翻转一次——存储件的时钟就是它。
- **其余 14 个**：`0x6a` Input 与 `0x3e` Switched Input 按**源**（值来自关卡/游戏状态数组）；
  `0x53`/`0x33`/`0x59`/`0x57`/`0x4d`/`0x75` 按**观察者**（游戏对它们一行代码都不生成）；
  `0x29`/`0x3b` 是关卡地图件（生成的是 `level 值 & 引脚`，沙盒没有关卡值，故输出恒 0）；
  `0x32`/`0x5a`/`0x5e` 是常量源；`0x2f` 是"2n 位劈成两个 n 位"的字宽 Splitter
  （新器件 `deviceWordSplit`，n 由导线解出）；`0x35` 与 `0x36` Load port 引脚逐条相同，
  按 Load port 处理。
- **门槛**：`simcore` **357/357**、查看器 **188/188**；新增真机用例 `sandbox-storage`
  （`tests/sandbox-storage-playtest.ps1`）把一块常量 + 寄存器 + 计数器 + 延迟线的板子部署成
  沙盒关卡，断言加载器不再拒绝它、并且 `devices -> u2_0_register, u3_2_counter, u4_1_delay,
  u1_constant` 四个器件都在。离线测试当场抓到的一个真 bug 是位序（`BitVector` 是 MSB-first，
  而从元素 0 开始进位会数出 `10000000`）。
- **没量到的地方照旧写清楚**：`0x5a` Static Value 的"值"存在哪个字段仍是开放问题（记录宽度
  恰好等于生成的值，改宽度会让游戏丢弃该元件）；`0x57` Halt 在沙盒里按观察者处理（没有"停"
  这个概念）；关卡件要跟随关卡值需要先有"关卡值"这一层。详见
  [simcore-kind-coverage.md](research/simcore-kind-coverage.md) §7.5。

## 未发布：元件编辑器 V4——调色板、混合取色、校验标记与确认对话框（2026-09-29）

- **`tc.component.editor` V4（`TC_COMPONENT_EDITOR_API_VERSION_4`）**：把 V3 留下的三件补齐，做法
  全是加法——字段描述符仍是 `TCComponentFieldV2`，V3 的表一字未改，V3 成员在 `TCComponentEditorApiV4`
  里的偏移逐个不变（ABI 基线 1763 → 1785，新增 22 条，无一条已有记录被改）。
- **调色板**：`TC_COMPONENT_FIELD_COLOR_PALETTE`（位 8）让一行颜色画成 Mod 自己声明的**色块条**；
  色块复用 `choices`/`choice_labels`，每条是一条颜色字面量（`#RRGGBB`/`#RRGGBBAA`，`#RGB` 按 CSS
  展开，前缀可省），最多 16 个。宿主逐块发布矩形，点一块提交一块。
- **混合取色行**：`TC_COMPONENT_FIELD_COLOR_HEX`（位 9）在同一行再加一个文本框，内容是打包值
  `0xAABBGGRR`（也接受颜色字面量）——颜色字段原本没有的精确位型出口。两个位同时声明就是
  "色块条 + 十六进制框"。
- **一行式校验标记**：`TC_COMPONENT_FIELD_VALIDATION_ICON`（位 10）把校验消息从"占一整行文字"
  改成控件末尾一个小方块（错误红 / 警告琥珀），消息进 tooltip 与日志。标记用窗口自己的绘制表画，
  不提交 ImGui 项，所以行的排版与行高一个像素都不动。
- **确认 / 取消式多字段编辑**：表级 `TC_COMPONENT_EDITOR_TABLE_CONFIRM`（经新的
  `TCComponentEditorApiV4::set_fields_v3()` 传入）把整张表改成**暂存**：值落在宿主的副本里、行上
  显示暂存值、校验照跑，行下多出 Apply / Cancel。Apply 是**一次配置写**——不管改了几个字段都只有
  一步撤销和一次 `on_config_changed`；Cancel 重读实例、丢掉暂存。声明了 CUSTOM 行的表被拒绝
  （那种行自己经 `tc.component.storage` 写配置，会和副本抢同一个 blob），并写明是哪一条行。
- **版本纪律**：`TC_COMPONENT_FIELD_V3_KNOWN_FLAGS` 把 V3 的位集单独留名并逐位写出，
  `TC_COMPONENT_FIELD_KNOWN_FLAGS` 继续是"当前这一版"的别名；V3 入口按前者校验，所以新加载器不会
  因为自己长大就悄悄接受一个 V3 调用方不可能知道的位。（第一版把它写成前者到后者的宏别名，于是它
  在使用点展开成 V4 的位集——离线用例当场抓住，见
  [HANDOFF-component-editor.md](HANDOFF-component-editor.md) §8。）
- **示例与门槛**：`examples/editor-demo` 加一条"色块条 + 十六进制框"的混合行、给两条已有校验器
  的行装上标记，并加第三个类型 `EDIT_003`（表级 CONFIRM）；`tests/component-editor.cpp` 覆盖取色
  字面量的解析与格式化、位集拒绝、表级开关的拒绝项；真机用例新增 `component-editor-swatch`、
  `component-editor-color-hex`、`component-editor-validation-icon`、`component-editor-confirm`、
  `component-editor-confirm-cancel`（编辑器用例 13 → 18 条）。

## 未发布：M4 余项——引脚通道参数与译码器起步（2026-09-29）

- **`tc.component.compose` V2 新增 `pin_lane(session, cells)`**：把组合类型自己的引脚通道拉开
  （1–16 格，默认 2 = 历史形状 -18/+13，既有形状一个字节不变）。输入列向左、输出列向右，中间空出来
  的就是内部电路的画布——宽接口元件（译码器这类）要靠它布局。声明式 API 早有同名旋钮且宽通道的导线
  位置已验证，组合类型直接复用。真机：2 入 / **32 出** + 通道 16 的类型注册成功，游戏读回两列引脚
  相距 **31 格**（`in0=(-15,0)`、`out0=(16,-15)`；输入侧少一格是游戏对自己 ±3 连接偏移的取整，
  棋盘读数同值）。
- **同网络总线**：builder 现在只拒绝**不同网络**的相交，同一源脚扇出的线允许相接、可以画成干线加
  分支（游戏里相触的线本来就是同一网络）。离线 `tests/component-netlist.cpp` 的 bus 用例通过。
- **5→32 译码器**：`examples/composite-decoder/decoder.hpp` 的网表生成器（5 个 1 位译码器 + 60 个
  二输入 AND）与离线真值表模型写好并通过；**布局**仍未完成——按上面的通道参数改成宽通道下的
  从左到右树形布线是下一步（用显式路径点，让生成器自己画走廊），离线校验器先跑绿，再接真机门禁。
  `tests/component-decoder.cpp` 因此在 `test-catalog.json` 之外。

### 同日收口（2026-09-29 下午）

- **布线判据订正**：上面的"同网络总线"只对了一半。玩家给出的规则是**一根导线只有两个端点**：
  在导线中间放端点会把它拆成两根、新端点同属两者（所以那是同一个网络），而**两根线交叉、交叉处
  没有端点时是两个网络**。原先"相触即同网络"的判据因此过严，会把游戏本来就接受的布局判成错。
  现在只拒绝**端点落在别的网络上**（等价于放一个端点把两根线接成一个网络）与**共线重叠**，交叉
  放行。测量与订正过程见 [HANDOFF-component-m4.md](HANDOFF-component-m4.md) §5.1；
  `tests/component-netlist.cpp` 新增 `routing rule` 一节，逐条钉住这四种情形（交叉合法、端点落上去
  拒绝、共线重叠拒绝、同源扇出两件事都允许）。
- **5→32 译码器落地**：生成器改用宽通道（`pin_lane` 16），并把树的**根换成 A4**——线号由此等于输入
  字（A0 为最低位）。原先从 A0 起树会把位序反过来，真值表 32 组里错 24 组；这个错一直被布线拒绝
  挡在前面，判据放宽之后才第一次暴露出来。`tests/component-decoder.cpp` 现在进 `build.ps1`：
  65 个零件（5 个 1 位译码器 + 60 个二输入 AND）的网表能布线、能编码，32 组输入的 one-hot 真值表
  全过。真机门禁（导入 + 游戏重算门数 + 关卡真值表）还没跑；元件形态见下面的探针。
- **元件形态的探针**：`tests/decoder-probe.cpp` 把同一份网表经 `tc.component.compose` V2
  （begin / node / connect / commit）注册成**组合元件**，`build.ps1` 把它打成
  `dist/dev.decoder-probe.mod`。它先跑一遍离线校验，布局不合规就拒绝注册并写日志，不会留下半注册
  的类型。
- **沙盒的观测接上游戏侧（S4 第一件）**：`examples/sandbox-sim` 加了 `vcd=1` 与 `vcd_cycles=N`
  （默认 60）两个开关——绑定一块板子后跑够 N 个周期，就把引擎自己的 trace 用
  `tcsim::writeVcd()` 写成标准 VCD 到插件数据目录 `sandbox-sim.vcd`，并记一行
  `sandbox-sim: wrote … (N event(s), M nets, K violation(s))`。它与离线 iverilog 交叉验证比对的是
  **同一份事件表**，示波器和棋盘读的也是它，所以外部波形与屏幕不可能各说各话。违例的面板/棋盘
  高亮那一半还没做。另记一笔位置：开关文件是**插件数据目录**里的
  `tc-modloader-data/plugin-data/dev.sandbox-sim/sandbox-sim.txt`（先前文档里写的
  `tc-modloader-data/sandbox-sim.txt` 是错的，已改）。
- **与 simcore-view 同步**：同一个插件现在把游戏正在跑的板子按 `simcore/view` 的板子记录格式
  写到 `sandbox-sim-board.txt`（`dump=0` 可关，默认开；绑定板子时写一次，之后约每秒刷新），
  于是 `build\simcore-view.exe --board <那个文件>` 就能用**另一套渲染管线**（GDI，不是游戏的
  ImGui）画同一块板子，点一根线还能看到它的 net 值与写回的槽位——这正是分辨"游戏渲染错"还是
  "simcore 算错"需要的对照。查看器暂为打开时读一次（`O` 键换文件），跟随刷新的 `--watch` 未做。
- **实时跟随 + 沙盒自动拉起**：`simcore-view` 新增 `--watch`（每 200 ms 看一次记录文件，变了就
  重载，保留视角与选中），沙盒插件在绑定成功后如果当前是沙盒就自己 `CreateProcess` 起
  `simcore-view.exe --board <记录> --watch`。viewer 已直接打进 `dev.sandbox-sim.mod` 的 `native/`
  目录，插件按自身 DLL 路径找到解包后的 exe，安装 Mod 后不再需要手工复制。已有 viewer 不重复开，
  关闭后下次进入沙盒可重开；`view=0` 关掉、`viewer=<路径>` 覆盖随包版本。真机门槛会核对启动日志、
  `--watch` 参数和进程实际路径确实来自本次沙盒的插件缓存。
- **画游戏真正画的那条线（记录带 `path=`）**：运行时导线记录的 `+0x20` 是腿数、`+0x28` 指向腿数组
  （Nim seq：count/reserved/data，每腿一对 u16 =（方向码, 长度））。方向码是"绕原点逆时针每 45°"
  八个方向中的四个实测值（`0=+x`、`5=(-1,+1)`、`6=(0,+1)`、`7=(+1,+1)`，腿空间 y 向上），三条线的
  腿求和与端点差**逐条相等**，所以不是猜的。插件把它换算成绝对点写进板子记录的 `path=x,y;x,y;…`，
  查看器有这个字段就照它画、没有才用自己的布线器。离线判据：带 `path=` 的图里出现了斜线
  （查看器的布线器只会画直角），不带则是它自己绕出来的一堆直角。
- **查看器认 `local.clock` 的三个 Mod 源**：自定义原型的身份在 `custom_id` 里而不是 kind 字节里
  （三者都是 kind `0x4e`），所以之前查看器一律"无引脚几何"并拒绝跑。现在按 id 认领 `CLOK_001`
  /`SWIT_001`/`BUTN_001`，几何用**游戏自己报的**读数（`custom 3 id=0x535749545f303031 at
  (-15,-18) … out0=(-13,-18)`，按钮同形）：0 输入、1 输出、输出在 (+2,0)。时钟进 clock_nets、
  开关/按钮进 source_nets 的语义 simcore 早就有，缺的只是这张表。
- **交互源的电平进记录**：自锁开关/按钮的电平存在**宿主持有的实例配置**里（点击写进去），不在
  状态数组里，所以查看器自己重跑仿真时源默认是 0——游戏里拨到高，view 里还是低。现在插件的组件
  行会带 `id=<实例> level=<0|1>`（就是它喂给 simcore 的那个值），查看器给源网加激励时优先用记录里
  的电平，没有才退回它自己的测试模式。判据：同一份记录加上 `level=1` 后那条网从蓝（0）变绿（1）。
  时钟的相位还没带过去（它走 clock_nets，由查看器自己产生），是已知的下一条。
- **锁存器的"记忆"进记录**：锁存器的状态就是它的过去，而查看器是从零重跑一遍——所以即使开关电平
  被带过去了，中间那两条反馈线仍然停在 X。现在插件给每条导线写 `value=<0|1>`（就是游戏正在显示
  的那个值，来自它每周期写回的游戏状态数组），查看器把**源网**按这个值激励、把**反馈网**按这个值
  上电种子，于是"先前设好的状态"在 view 里也会被记住。判据：同一块 NAND 锁存器、输入 (1,1)，
  带 `value=` 时保持 1/0（绿+蓝、无红），不带时一半是 X（红）。
- **自定义元件的引脚与储存件（同一天，两件）**：
  - **引脚随记录走**：自定义原型（kind `0x4e`）的引脚只有游戏知道，插件的组件行现在写
    `inpins=x,y;… outpins=x,y;…`，查看器把它们注册成一张"按实例 id 查"的表（绘制与建网表共用）。
    于是**任何** Mod 元件都能画出来，不用在查看器里逐个硬编码。离线判据：一个表里从没有过的
    id（12345）现在画成 "Mod" 并带真实引脚，之前是 `no pin geometry in the frozen table`。
  - **储存件**：加了三个**声明了引脚**的储存类到冻结表 —— `Auto Register (0x32, in (0,-2)/out
    (3,0))`、`Auto Delay Line (0x77, 同形)`、`Counter (0x4d, 三个输入)`。其余储存类（Delay Line
    `0x0d/0x37`、Register `0x0e/0x27`、Counter `0x26`、RAM `0x76`）在目录里**声明 0 个输入脚**，
    输入位置未实测，按红线 6 不猜、先不加。
  - 仍未支持的是**行为**：几何有了之后，simcore 仍然拒绝对一个它不认识的 Mod 类型求值（日志
    `FATAL: custom component … has no sandbox simulator behavior`）——`local.clock` 的三个源是
    已知类型所以能跑，其余自定义件要跑起来，需要把它的内部电路交给 simcore（对玩家自制件，那份
    电路在游戏手里），那是独立的一刀。
  - **内存与其读写端口（同日晚些时候）**：先完成**几何**。测量工具本身有个洞：
    `tests/kind-list-probe.cpp` 与
    `tests/compose-probe.cpp` 都只印前 4 个引脚，Store port 的第 5 个输入（`(-1,0)`）正好落在
    这个洞里；两个 dump 都放宽到 8，`component-compose` 的真机用例（两条独立的读表路径逐字段
    对照）仍然全绿。补进冻结表的是 `0x36 Load port`（3 进 2 出）、`0x38 Store port`（5 进 1 出）
    与 `0x76 RAM`（**0 进 0 出**，不是遗漏：游戏自己的原型表就写 0/0，Symphony 那关的棋盘
    `campaign/symphony_6_ram/circuit.data` 也只有 2 个 RAM、6 个 Load port、4 个 Store port 和
    **0 根导线**——端口直接摆在 RAM 的插座格上，内存是靠端口接进电路的）。离线判据：
    `simcore/view/tests/view-tests.cpp` 新增 `memory geometry` 一组（引脚逐条对照，并断言 RAM 与
    插座端口可以构成一个可运行的共享内存）。
  - **内存行为已接入**：游戏自己生成的 native logic 源码确认容量为
    `2^(+0xe0 + +0xf0)` 字节、端口宽度为 8/16/32、起始地址先掩码、跨末端使用零 guard，且插座
    从上到下执行（同址 Store 上方的 Load 读旧值，下方读新值）。simcore 新增稀疏 RAM 私有状态、
    Load/Store 器件和隐藏顺序链；宽外部激励与状态槽写回也从 1 bit 修成完整总线。板子记录携带
    `memory_bits/memory_extra/little_endian`，viewer 重放不会把 16/32 位端口降成 8 位。
    runtime 在每个游戏周期边界切换私有顺序 token，因此地址/使能保持不变时端口仍会重新执行；
    第二周期的上方 Load 会看见第一周期 Store 写入的值，不会永远卡在上电读数。
    真机用例 `tests/sandbox-memory-playtest.ps1` 还新增 `-CircuitPath`，可复测玩家存档。当前剩余边界
    只有关卡 `memory_manager` 的 reset-data 初始化桥；没注入时 RAM 从全零开始。测量与边界写在
    [PLAN-sandbox-simulator.md](PLAN-sandbox-simulator.md) §15.3 与
    [research/board-object-fields.md](research/board-object-fields.md) §7。
- **布线不得穿过元件本体**：检查器原先只看脚点与别的线，于是宽布局的线会直接从元件身上压过去
  （能用，但画出来是错的；真机在元件工坊里一眼看得见）。现在每个内层节点按**引脚包围盒 + 一格
  净空**算一块区域，只有"接到自己那个引脚"的那一段允许进入，其余任何一段落在里面都判
  `Clearance`。`planRoutes` 同样按这条选路，所以现有网表是绕开的而不是被拒。5→32 译码器重新
  布线后仍然 32 组真值表全过。
- **组合元件能在元件工坊里打开**：工坊打开某个元件时，是拿**名字**去玩家那份电路管理器里找
  `<存档>/schematics/foundry/<名字>/circuit.data`，而 Mod 类型在玩家这边没有这个文件，所以那个
  "在元件工坊中编辑"按钮只会开一个空定义（这也是 V4 `set_foundry_button` 存在的背景）。真机
  测量（2026-09-29，用户存档）确认：那个文件就是**元件定义**格式——同一套头部与引脚记录，把
  我们的定义字节写进去，工坊立刻画出 65 个零件和 157 根线。于是加载器在注册组合元件时**补上
  这个条目**：文件不存在或是游戏自己建的空占位（≤64 字节）才写，玩家改过的一律不动，并在日志里
  记一行 `foundry view: filled …`。
- **simcore 补上游戏自己的字宽 Mux（`0x2a`）**：真机沙盒里正在跑的那块板上就有一块
  （`sandbox-sim-board.txt` 元件 22，`word30=3 word40=1 memory_bits=32`），而在这一刀之前
  simcore 递给它的是 `no device in the library`，整块板因此拒跑。现在 `devices.hpp` 有按位宽
  缓存的 `deviceMux`（`S(1)/A(w)/B(w) → Y(w)`，行为是逐位的 Verilog 条件算子；目录里原来的
  `mux2` 成为它的一位别名，语义只有一处），`board.hpp` 把"自动宽度"从 Word Switch 一处推广成
  一张表：先取记录 `+0xe0`（游戏算好的宽度），记录里没有才取数据引脚上最宽的导线，**选通/使能
  这类 `w1` 控制脚永远不参与加宽**。引脚顺序与宽度全部有据——游戏原型表
  `in0=(-1,-1,w1) in1=(-1,0,wauto) in2=(-1,1,wauto) out0=(2,0,wauto)`、同一次真机 dump 的导线
  （1 位线落在 in0）、关卡 `byte_mux/test.si` 的 `select==1 → B`、以及已验过的 `shape_mux8`
  元件形状。交叉验证新增第八个电路 `mux_word`（4 位 A/B + 选通跳 X 的一拍），iverilog 13.0
  逐网一致；查看器把那条真机记录固化成 `simcore/view/data/mux.txt`，离线用例断言重放出来的是
  1 位选通 + 32 位数据的 `mux32` 而不是拒跑。门槛：`simcore` **234/234**、`simcore-view`
  **180/180**、`simcore-iverilog` **8/8**。顺带量到一条新偏差并记账：多位网的成对弧延迟
  iverilog 按新值**最低位**选边、本内核按最高位，含 x/z 的值两家都没有可归纳的规则
  （`simcore/tests/probes/probe4-vector-delays.v`，结论写在计划 §13.3 与 §16），因此总线器件
  一律用同一个数做弧延迟交叉验证，而不是照抄一方。
- **屏幕、Time、Keyboard 进 simcore（同样是 2026-09-29）**：真机那块板子上三件都有，而之前
  它们是三条 fatal（查看器 `no pin geometry`、构建器 `no device in the library`）。语义全部取自
  游戏自己的文件：原型表给了三个脚（`0x5b Screen inputs=3 in0=(-1…w1) …`），关卡对话给了名字
  （Enable / Setting index / Value）与"Enable 要是 [T]"，关卡的 ISA 给了写法（`screen %a, %b` =
  把 %b 写进设置 %a），**手册 `asset/manual/Circuits/The screen/doc.txt` 给了全部语义**——屏幕显示
  的是它链接的那颗 RAM，四种模式（ASCII 8 / ASCII 32 / Pixel 8 / Pixel 32）、像素格式
  （`RRRGGGBB` / RGB+填充）与分辨率公式 `4×(setting+1) × 3×(setting+1)`；关卡自己的程序也照着用
  （`screen 2 := 19` → 80 像素宽，绘制代码每行正好 80 字节）。落点：新增 `tcsim/screen.hpp`
  （设置表 + 锁存器件 + `renderScreen()`，Pixel 两种模式可实现，ASCII 两种明说缺游戏字库不画假图），
  `board.hpp` 建出屏幕器件并把设置表、三个网记进 `BuildResult::screens`（输入网同时进
  `observed_nets`），Time/Keyboard 按宿主源处理（值只有游戏知道，simcore 只当影子；顺手修掉
  "源种类也会把自己的输入脚标成源"这条对 Keyboard 才暴露的问题）。门槛：`simcore` **255/255**、
  `simcore-view` **188/188**。**还没到"画出画面"**：屏幕画的是被链接 RAM 的字节，链接字段与
  RAM 内容这两件数据要么走离线桥（链接 + `memory_manager` 只读桥）、要么走在线桥（转发
  `screen_mesh` 的 `set8_bit_pixels`/`set32_bit_pixels` 像素），两条路与测量方法写在计划 §17.3。
- ABI 基线 1763 → **1764** 条（V2 表追加 `pin_lane`）。

## 未发布：M4 第二～四刀——内部网表 builder、嵌套与原版复用三路径

第一刀把原版类型目录服务化了；这一轮把 M4 剩下的三件做完，并按计划 §8.3 把"完整别名"这条
**明确判定为不可用**，而不是发一个只有外观的空壳。

- **`tc.component.compose` V2：内部网表 builder**。`begin`/`node`/`connect`/`commit`/`abort` +
  `status`：按稳定拼写建节点（`builtin/…`）、连引脚、提交时校验并导入。定义本身就是内部电路，
  游戏像编译手画电路一样编译它——所以**门数由游戏按内部电路重算**（真机：两个 AND 节点 →
  `gates=2`，而探针故意声明 1，这个差额就是"游戏真的走了网表"的证据），`delay` 则原样信任。
  编码器写在 `src/component_netlist.hpp`，离线用例 `tests/component-netlist.cpp` 把同一个
  AND3 网表的字节与**游戏已经接受过的夹具定义** `build/nl_def_and3.data` 逐字段对齐。
- **校验**：空网表、引脚越界、同一脚重复驱动、内部输入没被驱动（外层输出脚也算 sink）、
  无状态组合环（经过"不声明输入脚"的零件的环合法——这就是 Delay Line 反馈在游戏里的形态）、
  以及布线清扫（会穿过别的引脚或与别的线相交的布局拒绝，并把冲突点写进 `status`）。
- **嵌套**：`node()` 也接受另一个已注册类型的 `type_id`（`<mod>/0x<id>`，或直接 `0x<id>`），
  内层引脚从它自己的原型读——真机探针把一个组合元件嵌进另一个组合元件并成功导入。
- **复用三路径**：`create_builtin_node` = V2 的 `builtin/…` 节点；`clone_builtin_metadata` =
  `clone_metadata(custom_id, "builtin/…", NAME|DESCRIPTION|COST)`，真机把 `builtin/AND` 的名字与
  说明抄到一个组合类型上，游戏存的原型名字就是 `AND`、说明就是它自己的 `[T] when both inputs are [T]`；
  `register_builtin_alias` **恒返回 `UNSUPPORTED`**（`TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED`），
  理由写进契约：游戏按 kind 字节分发、Mod 实例恒为 `0x4e`，另有两条既有实测（自定义原型忽略
  shape SVG；默认绘制只能按类型关掉）指向同一结论。目录新增 `TC_BUILTIN_FLAG_ALIASABLE`，本构建
  从不置位。替代方案是"名字/说明克隆 + 同一个原版节点拼组合 + V5 图片"。
- **能力位**：`tc.component.registry` 现在为组合类型报 `implementation=COMPOSITE` 并置新的
  `TC_COMPONENT_CAP_COMPOSITE`（真机 `capabilities=13` = LOGIC|MULTI_PIN|COMPOSITE）。
- **真机门禁** `component-composite`（`tests/component-composite-playtest.ps1` +
  `tests/composite-probe.cpp`）：注册 AND3 → 游戏读回 `in=3 out=1 gates=2 delay=2`；把
  `build/nl_and3board.data` 放进 `and_gate_3` 关卡，**游戏自己的关卡测试判 `verdict=1`**；嵌套
  组合也导入成功；坏网表被拒并给出人话理由；`clone_metadata` 与 `alias` 的结果都在日志里。
- **顺手修掉一个真 bug**：`tc.component.compose` 的 V1 读接口把服务 context 当成了运行时指针
  （V2 的写接口要用插件指针），两者混用后 V1 调用方会解引用错对象——正是复合探针第一次跑就挂在
  `begin` 里的原因。现在读/写接口统一以插件为 context、经 `owner` 取运行时，V1 与 V2 两张表都
  有真机门禁（`component-compose` 与 `component-composite`）。
- ABI 基线 1718 → **1763** 条（V2 结构、`TCComposeTypeV1`、错误码、克隆标志、能力位）；文档更新
  `docs/sdk/services.md`（V2 与三条路径）、`reference/limits.md`（builder 规则表）、
  `docs/HANDOFF-component-m4.md`、`PLAN-custom-components.md` 的 M4 行。

## 未发布：M4 第一刀——原版类型目录（`tc.component.compose` V1）

计划 §16 阶段 4 要的是"Mod 用游戏自己的零件拼元件"：稳定原版类型目录、内部网表 builder、嵌套依赖、
完整别名。这一轮交付第一项，其余写进交接文档。

- **新服务 `tc.component.compose` V1**（`sdk/tc_component_compose.h`）：引导时把原型表读一遍，
  给出每个 kind 的**稳定拼写**、名字、说明、声明的引脚（板面格偏移 + 原始字宽/自动标记）与两个
  标志（`NO_DECLARED_INPUTS`、`HAS_SHAPE`）。查找有 `get`／`find(kind)`／`find_by_type_id`，
  引脚单独取；读不到原型表时**不发布**这张表，而不是发一张空表。
- **拼写规则是本轮真正的新契约**：`builtin/<名字>`；同一构建重名按 **kind 升序**编号
  `#2`、`#3`…；没名字的槽位用 `builtin/0x<kind>`；`find_by_type_id` 先精确匹配，再对**无编号**
  拼写做 ASCII 大小写折叠，编号拼写永远精确。真机实测本构建 125 个 kind 里 **17 个名字重复**
  （`Output` ×7、`Input` ×6、`Splitter` ×6、`Word Maker`/`Concatenator`/`Maker` ×3，其余 ×2）、
  **21 个槽位无名**——按名字找零件必须靠编号，这条规则就是为它写的。
- **引脚宽度原样三态**：1–64 报 `bits`；`0x7fff…` 报 `AUTO_WIDTH`；本构建两个 Splitter 用的
  `0x7fff…fe`／`…fd` 未核实语义，报 `RAW_WIDTH` 且 `bits=0`。**不猜**。
- **代价与能力位故意不报**：内置零件的门数/延迟是游戏打分器从内部电路算出来的（实测 Delay Line
  5 门 4 延迟），原型里那两个字段本构建全是 0；能力位属于别名那条路要做的研究。
- **离线**：`tests/component-compose.cpp`（`build.ps1` 内执行）——拼写、重名编号的确定性、
  大小写折叠的边界（编号不参与折叠）、两个计数、目录 ABI 的字符串容量与字段顺序。
- **真机**：`tests/component-compose-playtest.ps1`（game 层，用例 `component-compose`）把新探针
  `dev.compose-probe` 与既有的引脚表探针 `dev.kind-list` **装进同一个沙盒、同一次会话**跑一遍，
  再要求两个互相独立的读表者（SDK 的 `TCGameModel` 封装 vs 加载器自己的 `prototypeGet`+引脚解码）
  逐条一致：kind、名字、引脚数、每根引脚偏移与原始字宽——**125 个 kind 全对上**。任意一边悄悄
  漂移都会变成 mismatch，而不是悄悄通过。
- ABI 基线加 `tc.component.compose` 的常量、两个结构与查询表；文档
  `docs/sdk/services.md`（服务一节）、`reference/limits.md`（目录规则）、`docs/HANDOFF-component-m4.md`
  （本轮范围与下一刀路线）。
- **顺手修掉一个测试脚手架漏**：`game-handle-probe-playtest.ps1` 复用沙盒 profile，而它的驱动最后会用
  游戏自己的"保存关卡"命令把棋盘写进 profile——第二次运行就会把那块板读回来，`live-before` 变成 1、
  编辑相位落在被占的格子上（`FAILED/STATE`），表现为"这个用例时好时坏"。现在该用例加 `-Recreate`
  （profile 每次重建），连跑两次都过。别的用例要不要同样处理，取决于它们是否也依赖 profile 里的存档。

## 未发布：命令总线补完（可撤销删除元件 + 类型归属）

P1 编辑命令族里"删除元件"一直是空着的：低层 `board_delete_component` 只把记录变成 kind 0 墓碑，
没有撤销条目，所以上一轮只做到"入口已核实"，没进总线。这一轮把上一轮反推出来的撤销感知包装接上：

- **`TC_COMMAND_BOARD_DELETE_COMPONENT`（type 9）**：载荷是"实例 id + 类型"
  （`argument` = 实例 id，`custom_prototype_id` = 类型，`x/y/kind/rotation` 不用），执行时按
  `kind == 0x4e`、记录 `+8` 的实例 id、`+0x188` 的自定义 id 三条同时命中找到序列下标，用它和空
  导线列表造两个 8 字节整数序列（游戏自己的 `sequenceNew`），交给
  `try_delete__modelZutilities_u8585`，最后 `sequenceDestroy` 两个序列。目标不在棋盘时
  `FAILED/TC_COMMAND_ERR_NOT_FOUND`。
- **可撤销**：`try_delete` 先复制要删的记录、连挂在元件上的导线一起删，再把变更交给游戏的撤销栈。
  真机（`component-placement`，同一个沙盒）：
  `state=3 result=0 live=4->3 at(28,10)=0` → `board.undo` 后 `live=3 at(28,10)=1` →
  `board.redo` 后 `live=3 at(28,10)=0`。低层墓碑删除做不到这件事（撤回不来）。
- **归属规则**：入队前查元件目录（`tc::component_registry`）里该类型的 `owner_mod`，如果它记在
  **另一个** Mod 名下，命令 `TC_COMMAND_ERR_ARGUMENT` 直接拒绝（未入队，`request` 保持 0）。
  目录记不到的**原始电路导入**（Mod 自己用 `tc.component.model` 导入，`examples/circuit-and`、
  `examples/custom-or` 走的路）没有归属信息，命令照常执行——与 `DUPLICATE_COMPONENT` 一致，
  这两条路上加载器不承担"这是谁的元件"的判断。
- **真机门禁**：`component-placement` 用例扩了三步——用总线放一个 `local.editor-demo` 的类型实例
  （peer 包，公开的自定义 id），删它 → `submit=-2 request=0 still=1`（棋盘不动）；把同一个实例改
  报成没人注册过的类型 id → `submit=0`、执行 `state=4 result=-5`（NOT_FOUND）。契约、`limits.md`
  的命令表与 `commands.md` 的 V2 删除一节按实测写；ABI 基线加一条常量记录。
- **顺手修掉一个真 bug（事务旁路）**：`TCTransactionApiV2.stage` 的校验原先只覆盖 V1 的几条，既没有
  type 上界，也没有删除命令的归属检查——提交时 steps 逐条走同一个执行器，于是臆造的 type 会被
  `type<=TC_COMMAND_SIM_RESET ? "sim.do" : (undo ? ... : "board.redo")` 这条别名三元式落进 **redo**
  分支，另一个 Mod 的类型也能借事务删掉。现在 `submit` 与 `stage` 共用 `validateCommand`，transaction
  不可能放入总线会拒的命令；docs/sdk/transactions.md 按此更新。

## 未发布：字体资源服务化（render V8 的真机验证与一个真 bug）

计划 §10.2 把"字体资源"列为 M5 剩下的第一项：正文只能用宿主默认字体，斜体/自定义字体得插件自己画
（`examples/text-box` 就自己造了 atlas/换字体的 `FontOverride` 那套）。这一轮把 V8 的字体半真正跑起来：

- **一个真 bug（已修）**：这个构建的 ImGui 用 **32 位 `ImWchar`**（它自己的默认区间表是
  `20 00 00 00 / ff 00 00 00 / 0`），而宿主把 SDK 的 `TCComponentRenderFontRangeV1`（`uint32_t` 对）
  **按 16 位对**交给 atlas → 字形按另一组区间建成 → `measure_font` 还有度量（回退字形），
  但 `text_font` 一个顶点都不产生。现在按 32 位对编码。
- 真机门禁 `tests/text-box-playtest.ps1 -Font`（隔离沙箱，进 `tests/test-catalog.json`）：示例 Mod 用
  `load_font` 加载游戏自带的 CJK 面（BMP 全区间）与 UI 面（拉丁），断言两个面都被接受并报出
  句柄/native/revision；相对路径与超长区间被 `ERR_ARGUMENT` 拒绝；同一 key 再加载不变句柄且
  `revision=2`，释放后重载是全新句柄且 `revision=1`；`measure_font` 对面给出不同度量
  （CJK 42.6 px vs UI 62.4 px）；绘制路径本身用宿主默认面做像素断言（同一位置 102 个像素）。
- **仍未关闭**：由宿主加载的自定义面画出来仍是 0 像素（同一位置 `text_font(0,…)` 有 102 像素），
  而该 atlas 纹理作为一张图片四边形是能画出来的（151 像素）、`measure_font` 也按面返回度量。
  结论：这个构建里字体 atlas 的纹理 id 绑定还有一层没找对，下一步从
  `ImFontAtlas_SetTexID` 之外的后端路径查起。文档（services.md / limits.md / verification.md）
  按"资源半已验、绘制半未验"如实标注，不做已发布承诺。

## 未发布：实例与生命周期补完（放置、编译、解绑）

计划 §21 的 M2 只剩"`on_clone`、`on_load/on_save`（都已交付）、move/rotate/resize 与编译类回调"。
这一轮把后面两条补上，并明确区分销毁与解绑：

- `TCComponentLifecycleV1` 追加 `on_move` / `on_rotate` / `on_resize` / `on_compile` / `on_unbind`
  （按 `size` 生效，老调用方一个字节都没变）；
- `TCLogicIOV2` 追加**放置尾巴**：`cell_x` / `cell_y`（板面格）、`rotation`（顺时针 1/4 圈，就是
  棋盘记录里的方向字节）、`half_width` / `half_height`（占位半宽/半高，板面格）。老回调按
  `io->size` 判断是否可读；
- 宿主每帧读一次棋盘记录，只在**变化**时派发 move/rotate（第一次读数只是基线，编译不算移动）；
  逐实例占位写入处派发 resize；生成器产出这次编译的代码之后派发 compile；`releaseInstances(nullptr)`
  派发 unbind 而不是 destroy；
- 新相位：`TC_LOGIC_MOVE=9`、`TC_LOGIC_ROTATE=10`、`TC_LOGIC_RESIZE=11`、`TC_LOGIC_COMPILE=12`、
  `TC_LOGIC_UNBIND=13`。ABI 基线 1657 → 1672。

**真机证据**（`tests/pin-shape-playtest.ps1 -Shape wide9`，隔离沙箱）：

```text
pin-shape: wide9 on_compile instance=3689348814741910323 phase=12
pin-shape: geometry instance resize=0 (half=3,2)
pin-shape: wide9 on_resize instance=3689348814741910323 cell=-5,20 rotation=0 half=3.000000,2.000000
pin-shape: wide9 on_unbind instance=3689348814741910323 phase=13
```

**仍未关的门**：`on_move` / `on_rotate` 的宿主派发由离线 `tests/native-component.cpp` 断言
（基线、移动、旋转、footprint 不变不重复通知），但"棋盘记录真的被改了"这一步需要玩家拖动/旋转
元件——本构建对合成鼠标的棋盘拖动没有反应（与 2026-09-23 的 hitbox 研究结论一致），探针会把
尝试与结果写进日志，自动门禁待补。`on_compile_failed` 没有可靠信号（编译失败路径不经过生成器），
因此**不进契约**，留待编译错误模型。

## 未发布：元件属性编辑器补齐到 V3（下拉 / 颜色 / 自定义行 / 校验 / 面板约束）

玩家要求："先把属性编辑器补齐"、"界面不能超过游戏底部面板大小"、"排版要完善"、"收起底部面板时
不再渲染、再拉起时 UI 与已填内容不变"、"窗口变化时自动适应"，以及"底部属性栏默认不显示元件说明
文字，开发者可选打开"。这一轮把它们全部落地：

### `tc.component.editor` V3

- **枚举下拉**（`TC_COMPONENT_FIELD_DROPDOWN`）：真正的 combo，与 `RADIO` 互斥。
- **RGBA8 颜色行**（`TC_COMPONENT_FIELD_COLOR_RGBA8`，种类 8）：ImGui 自己的颜色控件，
  `0xAABBGGRR`；一次拖动 = 一步撤销。
- **自定义行**（`TC_COMPONENT_FIELD_CUSTOM`，种类 9）：宿主给裁剪画布 + `TCComponentRenderDrawV1`
  绘制表，Mod 自己画；`TCComponentEditorActionCallbackV1` 把右键菜单、双击、缩放抓手的按下/拖动/
  松手交给 Mod，事件带屏幕坐标、累计位移与配置快照。
- **行内校验**（`TCComponentEditorValidateV1`）：提交前把拟提交的整份配置交给 Mod；`ERROR` 拒绝并
  保留旧字节，`WARNING` 提交并记录，提示画在行下面。
- **编辑事务**：`begin_edit`/`commit_edit`/`abort_edit` 从编辑器服务转发到 `tc.component.storage` V2；
  缩放抓手按下即开事务、松手提交，**整段拖动 = 一步撤销**。
- **难点记录（都靠真机定位）**：ImGui 的 combo 弹窗先出现在指针处、下一帧才翻到字段上方，所以宿主
  把弹窗和每个选项的矩形写进日志，真机用例等它稳定后再点；自定义行的画布 item 会先抢走 ActiveId，
  抓手因此永远收不到点击——修法是给画布 `igSetNextItemAllowOverlap`；抓手本身加大到 14 px 起并夹进
  窗口内，否则行贴底时会被窗口裁掉。

### 面板约束与折叠（玩家要求 1/3/4）

- 行窗口每帧从游戏窗口读面板矩形，**左右上下都夹在面板内**（6 px 边距）：装不下时封顶在面板底边并
  交给 ImGui 滚动，只有放得下时才把面板上半留给游戏自己的内容。
- 钩子在折叠状态照常运行，宿主按"标题行以下还剩多少"（< 34 px）判折叠：**完全不画、也不动编辑
  状态**；重新拉起后行回到原矩形，先前填入的文本（`buffer=`）与预览不变。
- 面板矩形、字段宽度、窗口封顶每帧重算：窗口变窄时字段收缩而不是被推出面板。
- `TC_EDITOR_TRACE=1` 的 `window` 行新增 `panel=` 与 `toggle=`（游戏自己的折叠钮矩形），
  `popup`/`popup item`/`menu item`/`grip` 行公布各弹窗与抓手的矩形，真机用例据此精确点击。

### 说明文字默认不进面板（玩家要求 5）

- V2 定义表尾部新增 `flags`；`description` **默认不写进游戏原型**（游戏把它印在底部面板顶部，
  正是属性行所在处），设 `TC_COMPONENT_TYPE_SHOW_DESCRIPTION` 才显示。文字仍保留在加载器自己的
  类型目录（Mods 页）。旧的 V1 表没有 flags 字段、布局也不能长大，保持原行为并在日志里写明。

### 验证

- 离线：`tests/component-editor.cpp`（含 V3 的 kind/flag/表级规则与颜色、下拉互斥等用例）、
  `build.ps1` 全绿。
- 真机（隔离沙箱，全部进 `tests/test-catalog.json`）：注册（含两种说明文字结果）、复选框、文本、
  单选、十六进制、下拉（`RTZ` 并触发 `validator warned`）、下拉被拒（`RNA` 被 `validator refused`、
  配置不变）、双击、右键菜单（并写回配置）、缩放抓手（`RESIZE_BEGIN/…/END` 且拖动位移进配置）、
  颜色拖动、折叠/展开（折叠期间零行 trace、展开后同一矩形与同一 buffer）、32 行长表。

### 顺带修掉的两处构建/发布账目

- `dist/releases/0.8.1` 已经发布，但 `compat/profiles.json` 的 `acceptedInstalledLoaders` 只登记到
  0.8.0，于是安装器的"从已发布加载器升级"用例在 0.8.1 上被当作未知引擎拒绝（host 层红）。
  已把 0.8.1 的载荷哈希（`7a600aec…`）补进画像与 `src/setup_core.cpp` 的 `accepted[]`，
  用例恢复 `PASS upgrade from the published 0.8.1 loader`。
- `examples/float-ops/build-kernel.ps1` 的增量检查没有把 SDK 头当作依赖，改过
  `sdk/tc_component_types.h`（定义表尾部新增 `flags`）之后 `components.o` 仍是旧布局，离线兼容
  测试因此报 "definition size mismatch"。现在 SDK 下所有头都进依赖列表，头一动就重编。

## 0.8.1（2026-09-28）：Float Ops 扩到 binary16/binary64，元件栏按格式分层

### 自定义元件输入与渲染／仿真通道 V1（2026-09-28）

- 新增 `tc.component.input`：宿主做局部矩形命中、旋转/缩放坐标变换、逐实例 hover、pointer
  capture 与 keyboard focus，路由点击/双击/滚轮/拖动、物理键、Unicode 提交和 Windows IME
  preedit，并用回调返回值消费棋盘输入。
- 新增 `tc.component.channel`：generation-checked 实例的双向有界队列和双缓冲快照，带序号、
  DROP_NEWEST/DROP_OLDEST、统计、`ERR_BUSY` 非等待路径，以及实例/场景/Mod 生命周期清理。
- 新增 C++ SDK 封装、Windows x64 ABI 快照、旋转命中/两实例焦点/中文事件测试，以及 5000 条
  双线程 FIFO 压力测试。真实输入法候选窗、游戏快捷键穿透和两实例真机手感仍列为发布门禁。

### 研究：关卡接口——Mod 能不能创建新关卡（2026-09-28）

结论（全文与证据在 [research/level-interface.md](research/level-interface.md)）：

- **加载已有关卡**：能（`load_level__modelZutilities_u7740`，加载器已暴露为 `level.load`）。
- **给已有关卡换内容**：能，且不需要改加载器——关卡内容全在 `campaign/<名字>/`，而加载器的账本
  允许 Mod 包部署 `campaign/**`（`src/core.hpp` 的 `resource()` 白名单），带备份与事务回退。
  真机验证（`tests/level-content-playtest.ps1` + `tests/level-content-probe.cpp`）：在隔离副本里把
  `campaign/sandbox/{meta.txt,circuit.data}` 换成我们自己的对话与板子，游戏进的仍是 `sandbox`，
  但棋盘是两块 FP32 CONST → ADD → DISP 的我们那套（原版沙盒是空板），开场对话框也来自我们写的
  meta（原版沙盒没有 `dialogue` 字段）。
- **用一个新名字建关卡**：**不能只靠放文件夹**。关卡名编译在 exe 里（`always_on`×2、`byte_adder`×2、
  `binary_racer`×4），而 `load_level` 会先 `contains(campaigns…)` 再 `campaigns[…]` 查表；
  `find_all_levels` 与关卡树同样由编译表驱动。要新名字就得往游戏内存的战役表插条目（Nim
  Table/seq + 存档兼容），那是另一项研究。
- 顺带量到两条规则：**meta.txt 运行时解析**（字段名 `unlocks_components`/`post_mortem`/`hint_kind`…
  在 exe 里），而**关卡文本按 id 查 `translations/<语言>.txt`**（按文件分节，节内映射文本 id）——
  所以复用别人关卡的文字 id 会显示"别人的译文"（实测：第一次跑写英文、弹窗出中文）。

### 底部面板：藏掉元件预览图 + 修掉每帧刷屏的诊断日志（2026-09-28）

- **重进沙盒后输入框消失（真 bug，已修）**：玩家报"只有第一次进沙盒时输入框正常，退出重进、
  再选中元件，输入框就消失"。根因是**窗口绘制顺序**：加载器的属性行窗口用 `NoBackground` 画在
  游戏面板之上，靠"提交顺序在后面"才可见；而重进关卡时游戏会**重建底部面板窗口**，新窗口排在
  属性行窗口之后 → 面板的不透明底色把属性行整块盖住（行其实还在提交、也还在 trace 里，所以
  仅看日志一切正常，只有像素能看出问题）。修法：每帧在 `Begin` 之前调用 `igSetNextWindowFocus()`
  把行窗口显式提到最前（加载器自己的页面容器早就用同一招）。证据：`tests/float-reentry-driver.cpp`
  现在会在重进后**截一帧**（`panel.bmp`），修复后那张图里 `值 / 原始位 / 标签` 三行连同输入框
  都在——机制与修复前的差别是绘制顺序，所以这张图正是这条修复的验收物。
- **同一处修复的第一版把顶部菜单栏弄坏了**（玩家当天再报"打开底部面板就不能使用顶部菜单栏"）：
  `igSetNextWindowFocus()` 是**抢焦点**的调用，每帧抢一次会把玩家刚点开的菜单弹窗立刻关掉。
  现在只在**面板刚出现**的那一帧调用（用帧号差判断，`lastEditorRowFrame`），一次就够——ImGui
  会保持窗口顺序直到有别的窗口获得焦点，而面板只在关闭时（进关卡）被重建，正好落在帧号断档里。
  重进后的截图仍显示三行输入框，说明"提到最前"这件事只需一次。
- **属性行的标签与输入框离得太远（玩家 2026-09-28："输入框和旁边的文字离得太远，不美观"）**：
  标签列原来是面板宽度的 30%（面板跟屏幕一样宽，于是中间空出一大块）。现在列宽是**量出来的**：
  先给面板宽度 8% 的紧凑下限，再用 `igGetItemRectMax` 量每个标签的实际宽度去撑开，同一类型的
  各行共用这个列宽（按 custom id 记忆），输入框则填满该行剩下的宽度。第一版只用量出来的宽度，
  首帧列宽为 0、输入框压在标签上（截图自查抓到），所以加了那个下限。
- **不再向游戏注册元件说明（玩家 2026-09-28："去除底部面板中的元件说明"）**：游戏把类型的
  `description` 印在底部抽屉的标题下面，而那正是属性行的位置。`local.float-ops` 因此把
  `registered.description` 传 `nullptr`（加载器只在非空时调用 `setPrototypeDescription`，所以
  面板里连空行都不留）。说明文字本身没丢：`kFamilies` 表里的文本保持原样、README 逐类型记录，
  只是不再进游戏原型；如果以后想让它在元件栏提示里出现，把这一处改回 `entry.info.description`
  即可。
- **属性行的字号与位置（玩家 2026-09-28："这个输入框和对应的文字明显太大了，都挡住上面的元件
   名称了…排版要好看"）**：行窗口现在用自己的文本缩放（`igSetWindowFontScale(0.78)`，只作用于
   这个窗口，游戏和其它 Mod 的字号不受影响），输入框的高度随字号一起变小，三行读起来像一个
   表单；行窗口的顶边另外夹了一道下限——**不能高过抽屉自己的内容起点**（挂钩点就在标题行之后，
   光标位置就是标题下沿），所以面板变矮时第一行也不会再压住元件名。证据：
  `tests/float-reentry-game` 重进后截的 `panel.bmp`（`build/panel-smaller-rows.png`）。
- **输入框宽度（玩家 2026-09-28 同一天追加："这个输入框还是太大了，太宽了，可以适当缩小"）**：
  上一版让输入框填满整行，于是在跟屏幕等宽的抽屉里变成一条横杠。现在宽度**按文字高度封顶**：
  上限 `12 × 行文字高度`（`igGetTextLineHeight`；约二十来个拉丁字符，够放 `0x40200000` 与
  `1.7976931348623157e+308`），下限 `6 ×`，`fullRow` 的字段也走同一个上限——那个标志只表示
  "控件独占一行"，不再表示"占满面板"。行高、标签列宽与文字缩放（0.78）不变。证据：
  `build/panel-narrow-fields.png`。
- **同行字段（INLINE）把标签列顶飞（真 bug，已修）**：上一条的"量标签宽度来定列宽"读的是
  **上一项的矩形**（`igGetItemRectMax`）。普通行没问题，但 `TC_COMPONENT_FIELD_INLINE` 的标签是
  画在**左边那个字段之后**的，量到的是"配对待在哪儿"而不是标签有多宽，于是列宽每帧自增：真机日志
  （`tests/component-editor-rows` 的编辑器演示类型）里列宽 204 → 11981 → … → 八个 trace 帧后
  1.6 M 像素，输入框被推到屏幕外、点不到（driver 报 "did not finish"），`component-editor-rows` /
  `-text` / `-hex` 三条真机用例当场变红。
  修法：列宽与行高**改成问字体**（`igCalcTextSize` / `igGetTextLineHeight`，游戏自己画这些字用的
  就是这两个），同行字段的第二列另算一个偏移（`pairLabelX` / `pairFieldX`），成对的两个字段拿同样
  的一半宽度——全程只读文本度量，没有可回灌的状态，所以第一帧就是最终排版。float-ops 的抽屉像素
  与上一版一致（`值/原始位/标签` 三行、字段 192..612）。同时把 `component-editor-hex` 的断言改对：
  驱动点是**字段中心**，窄字段里插入点落在旧值中间（`beef` → `0xbeef0010`），用例本来就该断言
  "值里带着键入的数字"（驱动注释就是这么写的），而不是"以键入的数字结尾"。
- **元件预览图**：玩家反馈"底部属性栏里可以去掉元件预览图"。一个 Mod 元件没有可编辑原理图，
  这一点已经决定了工坊按钮的隐藏（V4），预览图是同一个事实的另一个面，所以两者**共用一个开关**：
   `setFoundryButton(id, false)` 之后，当被选中元件全是该 Mod 的这个类型时，加载器把底部面板
   画的两处图（`build_custom_component_preview` @0x14038ed85 与 `build_component_description_panel`
   @0x14039c916/@0x14039d336 的 `igImage` 调用）改成**全透明 tint 提交**：图仍然提交，面板布局
   与其它控件位置一个字都不动，只是看不见、也不再占视觉空间。元件栏卡片与放置幽灵不受影响。
   真机门禁 `float-panel-game` 增断言
   `Component render: the panel preview picture is hidden for the selected Mod component`。
   （玩家当天第三次追加"清掉引脚图示"之后，面板里那张图所在的**预览块**被整块跳过——见本节末
   一条；`igImage` 那个 hook 保留，它管的是有原理图的元件那条路径。）
- **诊断日志刷屏**：`Component panel: the drawer drew, …` 的判断把 `bool shown` 和"上次显示的
  custom id"直接比较，于是**每帧**都判为变化——一次真机用例写了 453 行，等于每帧一次文件 I/O。
  现在比较 id 本身，只在选中的类型真正变化时写一行。（这是玩家报"面板有时不跟手"时最值得先排掉的
  一类噪声。）
- **引脚图示（玩家 2026-09-28 第三次追加："清掉引脚图示"）**：预览图全透明、说明文字也不再注册之后，
   抽屉里仍留着一个 `— R`（截图里贴在 `原始位` 行右侧）。这次没有靠猜：先把绘制入口按
   "调用方模块 + RVA"打日志（`TC_PANEL_TRACE=1`，只在抽屉正画我们元件的那几帧记录），量到它
   **不在**十个 `igImage` 调用点、也**不在**六个 `render_box` 调用点上——它在 `R` 的真实出处：
   exe 的 `build_custom_component_preview__presenterZboard95uiZbottom95panelZcommon_u486`
   （0x38e990），那一支画的就是预览图（`igImage` @0x38ed8a）、引脚短线
   （`ImDrawList_AddRectFilled` @0x38efac）与引脚标签（`igText` @0x38f0ea）。所以这一版**整块不提交**：
   detour 只认抽屉那一次调用（0x39dc09 / 返回地址 0x39dc0e），闸门仍是"选中的元件全是该 Mod 的
   类型且隐藏了工坊按钮"。顺带把"预览图"那条也收进同一个动作里——图就在这一支里，块不提交，图与
   引脚图示一起消失，抽屉自己的标题、面包屑与花费图标都在抽屉那边，一个像素不动。证据：
   `build/panel-crop-preview-off.png`（重进后截帧，`R` 不在、三行输入框与标题都在），真机门禁
   `float-panel-game` 与 `float-reentry-game`。

### 加载器：宽度感知的十进制浮点字段（`TC_COMPONENT_FIELD_FLOAT_WIDTH`）

浮点元件的 FP16/FP64 常量一开始只能写十六进制（`0x2E66` 表示 0.1）——加载器的浮点行是 ImGui 的
32 位框，宽度写死四字节。这一轮把这个缺口补在**加载器**里，而不是让每个 Mod 自己再抄一遍文本
解析：

- **新字段种类** `TC_COMPONENT_FIELD_FLOAT_WIDTH = 7`（`sdk/tc_service_api.h`）：`bytes` 为
  **2/4/8** 字节，按 IEEE-754 binary16/binary32/binary64 解释，其余宽度整表拒绝。
- **行里显示**的是"能原样解析回该位模式的最短十进制"（逐位加长直到往返成功：binary16 最多 5
  位、binary32 9 位、binary64 17 位），所以抽屉里的数和棋盘上的数读起来一致；没人手写过的位模式
  （NaN、次正规、迁移写进去的值）也总有可解析的显示。
- **输入**接受普通十进制、科学计数法、`inf`/`-inf`/`nan`（任意大小写、可带符号）与 `-0`，**按本
  宽度正确舍入**；解析器没有整段吃掉的文本（空、`abc`、`1.2.3`、`3.5x`）一律拒绝，配置保持旧
  字节、输入框恢复成实例真正持有的值，并写一行日志。`min_float`/`max_float` 与 FLOAT 同规则钳位。
- **binary16 的两个转换自己写**（加载器里没有硬件半精度）：`halfToDouble` 精确，`doubleToHalf`
  按最近偶数舍入、该溢出到无穷就溢出；因为 double 的 53 位远超"十进制→binary16"两次舍入所需的
  2p+2 = 24 位，经 double 中转与直接舍入到 binary16 等价。**纯函数**在
  `tests/component-editor.cpp` 里离线钉住：具名拼写、**全部 65536 个 binary16 模式**的文本↔位模式
  往返、拒绝与钳位。
- **ABI 基线 1471 → 1472 条**（只多这一条常量记录，其余未变）。
- `local.float-ops` 的 FP16/FP64 常量因此和 FP32 一样有两行：`值`（十进制，新种类）+ `原始位`
  （十六进制，NaN payload 与精确向量的逃生口）+ `标签`。真机门禁 `float-widths-game` 用真实鼠标
  键盘把 `3.5` 打进 FP16 常量、把 `2.5` 打进 FP64 常量，并回读棋盘（`4.5 0x4480` /
  `3.5 0x400C000000000000`）。
- 测试驱动的小修：`tests/float-panel-driver.cpp` 在打字前先按若干次退格清空输入框。第一次真机跑
  这条用例时，`3.5` 被追加到已经显示 `1` 的框里，提交成了 `13.5`——这不是加载器的 bug，但"重打
  一个值"必须显式清空才说得清。

位宽那一轮只交付了**内核**（下面一条）；这一轮把元件补齐。定稿的取舍是**一个格式一个元件**
（`docs/PLAN-float-components.md` §6.4.1）：Mod 元件的引脚宽度在注册时按类型冻结，
`tc.component.types` 里没有"引脚宽度跟随实例配置"这一项，所以"同一个 Add 用左上角位宽框切换"
根本表达不出来——把它做成可切换类型只能绕开一半，而且 FP64 二元运算会正好压满 128 位输入预算。

- **65 个元件**：22 个类型族 × binary16/binary32/binary64，唯一的例外是没有 FP64 Fused Multiply
  Add（三个 64 位输入 192 位 > 加载器每个元件 128 位的输入预算；加载器会在注册时拒绝，所以它
  根本不注册，而不是注册后不工作）。`components.cpp` 的 `kFamilies` 表一族一行，注册表由它按
  格式展开一次（`catalogueStore()`）：引脚、描述、回调、配置布局与本体画法都只有一份。
  身份沿用 `F32…` 惯例推成 `F16…`/`F64…`（`floatTypeId()`，`static_assert` 钉住 22 个既有 id
  一个字节都没变），引脚宽度按格式给出（16/32/64 的字、1/5/10、1/8/23、1/11/52 的位段），
  整数侧仍是 32 位。
- **元件栏按格式分层**：名字从 `浮点/<分类>/<名字>` 变成 `浮点/<格式>/<分类>/<名字>`，
  也就是 `浮点 ▸ FP16 ▸ 计算 ▸ FP16 Add`。游戏按名字里的 `/` 建目录（
  `docs/research/palette-categories.md`），所以 65 个元件落在 3×5 个分类节点里，而不是挤在
  `浮点/计算` 一个节点下。`tests/float-palette-playtest.ps1` 除了一一核对 70 个 id（65 + 5 个
  M0 探针）之外，还断言三层目录本身存在。
- **回调怎么知道自己是谁**：`TCComponentTypeDefinitionV2::user` 现在装格式字节，加载器每个阶段
  都把它交回 `TCLogicIOV2::user`，于是一份回调代码服务三个同族类型（`ioFormat()`）；配置解码
  用**这个类型自己的**格式校验，另一宽度的 blob 会被拒绝并退回注册默认值，而不是被当成 16 位
  读进一个 64 位元件（`tests/float-compat.cpp` 用 1.0 + 2^-11 这个 binary16 半 ULP 用例把
  RNE/RUP 的差别与"外来配置被拒"都钉住了）。
- **FP16/FP64 常量的输入行是"原始位"**：加载器的十进制浮点行固定是四字节 binary32
  （`TC_COMPONENT_FIELD_FLOAT`），既不是 binary16 的两字节也不是 binary64 的八字节，所以
  新宽度的常量用可编辑的十六进制行写（`0x3C00` = binary16 的 1.0，
  `0x3FF0000000000000` = binary64 的 1.0），标签行照旧。binary64 因此有了自己的
  `ConstantConfigWide`（`+4` 起 8 字节位模式，标签在 `+12`），FP32 的旧 blob 一个字节不动。
- **文本层按宽度**：`fp/decimal.hpp` 增加 `format_*_for(format, …)` 四个入口。binary32 走原有的
  Ryu `f2s`；binary64 用同一份 vendored Ryu 的 **`d2s`**（新 vendored 文件，同一提交）；binary16
  没有 Ryu 入口，就用它的**精确** binary32 像的小数位，逐位截短到"能原样解析回同一个 binary16"
  为止——所以 binary16 的 0.1 显示 `0.1`，而不是 binary32 拼法的 `0.099975586`（65504 显示
  `65500`，与 Ryu 对最大 binary32 打 `3.4028235E38` 同理）。十六进制按格式给 4/8/16 位。
- **测试**：新增 `tests/float-text.cpp`（22 条具名拼写 + **全部 65536 个** binary16 模式往返 +
  20000 个 binary64 抽样往返 + 小缓冲安全），`tests/float-compat.cpp` 增加整表检查
  （22/22/21、每个类型的形状、引脚宽度、配置布局与默认 blob 的宽度字节、2+3=5 在三种格式、
  FP16 输入的高位不泄漏、Split/Make 逐宽度往返），`icons.cpp`/`build.ps1` 跟着从 22 张图变成
  65 张。加载器与游戏二进制仍不改。
- **真机门禁 `float-widths-game`**：一块只有三条 Constant → Add → Display 链（16/32/64 各一条）
  的棋盘，断言加载器自己的注册行（`…w16` / `…w64`）与三条显示各自格式的文本
  （`2 0x4000` / `2 0x40000000` / `2 0x4000000000000000`）。这条用例写出来第一次跑就抓到上面那个
  常量布局 bug：FP16/FP64 常量跟着 binary32 注册了 16 字节布局，逻辑回调退回默认值所以显示正确，
  但渲染读到 28 字节里的 `0x000000003F800000`，本体印出 `5.263544247e-315`。修法是
  `layoutOf(family, format)`（只有 Constant 依宽度选布局），并在离线整表用例里补上"默认 blob 是
  本宽度的 1.0"。

全绿：`examples/float-ops/build.ps1`（binary32 金向量 7571 条、文本层、宽度文本层、整表、
65 张元件图、打包），以及真机 `float-widths-game`、`float-palette-game`、`float-catalogue-game`、
`float-m2-game`、`float-package`、`float-icons`（其余 float-* 用例为回归）。

## 未发布：Float Ops 内核扩到 binary16/binary64（位宽的前半程）

浮点元件族的位宽（16/32/64）需要两样东西：一个能算三种格式的内核，和棋盘上那个位宽框变成
能改它的按钮。这一轮先交付**内核**，并把它的依赖按项目一直用的方法装进仓库：

- `examples/float-ops/fp/format.hpp` / `format.cpp`：binary16 / binary32 / binary64 共用一张
  `FormatOps` 表。binary32 **不重写**——表里的 32 位条目转调 `fp32.cpp`（有金向量与真机证据的
  那个内核），16/64 直接走同一份 vendored SoftFloat，共用同一套 `SoftFloatOperation` 的
  舍入模式与标志作用域；比较、分类、2019 min/max、位操作与 Split/Make 的位段按 `Layout`
  （符号/指数/尾数位宽）只写一遍，三种格式共用。
- vendored 子集从「binary32 的 17 个文件」扩到 **68 个**（f16/f64 的算术语义、整数转换与共享
  助手）。清单仍是**算出来的闭包**而不是手挑：编译整棵上游树 → `nm` 读每个对象的定义/未定义
  符号 → 从内核入口走引用图；方法与结论记在
  `examples/float-ops/third_party/berkeley-softfloat-3/README.md`。
- `tests/float-kernel.cpp` 增加宽度表一节：三种格式的 1/3、0.1+0.2、sqrt、溢出、精确次正规、
  饱和转换（D2）、有符号零的 min/max、Split/Make 往返，以及**32 位条目与 `fp32.cpp` 逐位一致**
  的对照。

**还没做**：位宽框的点击（它现在仍画死的 `32`）、元件按实例格式运算，以及 64 位值需要的引脚
宽度。顺序与取舍见 `docs/PLAN-float-components.md` §7.1/§7.2/§6.4。

## 未发布：`local.float-ops` 改用加载器接口、元件分类进文件夹、去掉工坊按钮

浮点元件族是最大的消费者，但它一直自己在游戏抽屉上画属性行——那正是
`docs/HANDOFF-component-editor.md` §1 说的"每个 Mod 抄一遍"的配方。这一轮把它收口，顺带做完了
玩家提的两处排列/按钮改动：

* **属性行改为 `tc.component.editor` V2 的声明式字段表**（`examples/float-ops/editor.cpp`，
  删掉 `components_ui.cpp` 的整扇行窗口、`panel-rows.txt` 诊断与自己的提交路径）：
  Constant 是 `值`（float，写它的 binary32 位模式）+ `原始位`（同一四字节的十六进制——NaN/Inf/次正规
  的逃生口）+ `标签`；Add 是 `舍入模式`（五选一 radio）+ `标签`；Display 是 `显示方式`（四选一）+
  `标签`；19 个目录类型按是否舍入给 `舍入模式` + `标签`。宿主画行、校验并走 `tc.component.storage`
  提交：**一次提交 = 一步撤销 + `on_config_changed`**。没有选择权的类型原来的只读说明行取消，
  那句话改由类型自己的 `description` 承担（游戏面板本来就在标题下印它）。
* **一条真正的功能性缺口也随之补上**：宿主提交不再经过 Mod 自己的 `writeConfig`，所以
  "提交后刷新板级求值 + 排队写电路存档"改由类型的 `on_config_changed` 负责；Constant 还要当场
  发布新值（暂停的板子否则会一直显示旧数字）。`tests/float-compat.cpp` 正好钉住这一条。
* **元件分类进文件夹**：名字从 `浮点/FP32 Add` 变成 `浮点/<组>/FP32 Add`，五个组是
  计算（10）、比较（4）、转换（4）、位与拆分（2）、常量与显示（2）；`浮点/M0/…` 五个探针不动。
  游戏按名字里的 `/` 建目录这一点早已实测（`docs/research/palette-categories.md`），所以
  `浮点` 下不再是 22 个平铺元件。
* **去掉底部面板的"在元件工坊编辑"**：22 个类型（含 M0 探针）全部调用 render V4 的
  `setFoundryButton(..., false)`——Mod 元件没有玩家可编辑的原理图，那个按钮只会打开一个空定义；
  宿主把它画成透明且不可点，面板布局不变。
* **测试**：真机沿用同一批驱动，但改读**加载器自己的行轨迹**（`TC_EDITOR_TRACE=1`，
  `Component editor: row instance=… field=… rect=…`）而不是 Mod 写的文件；两个驱动还补上了
  "等矩形稳定"和"按窗口可见部分点击"（宿主会把行窗口夹进屏幕，三行类型的最后一行会溢出边界）。
  顺带修掉一个测试侧的编码坑：Windows PowerShell 按 ANSI 读 `loader.log`，而 Subtract 那行的
  `value="−"`（U+2212）会解出引号、破坏布局正则——现在显式按 UTF-8 读。
  跑绿：`float-panel-game`、`float-busy-board-game`、`float-reentry-game`、`float-catalogue-game`、
  `float-palette-game`、`float-m2-game`、`float-icons`、`float-compat-game`、`float-boundary-game`、
  `float-pitch-game`，以及离线 `examples/float-ops/build.ps1`（kernel / 文本层 / compat）。

## 未发布：`tc.component.render` V7 —— 元件贴图由加载器持有

元件要在棋盘上画一张图，此前只有"插件自己干完"这条路：自己解 PNG（WIC + COM 单元）、自己
`create_ui_texture` 上传、再用 `engine_proc` 拿 `ImDrawList_AddImageQuad` 和背景 draw list 自己画。
`examples/clock` 的取值标记就是这么写的——自己的解码器、自己的 COM、宿主纹理句柄加三个引擎入口，
而它要的只是"游戏那张位精灵的第 0 行"。

* **新服务版本** `tc.component.render` V7（完整 V6 前缀 + 两个调用）：
  `load_image(context, key, png_path, out)` 与 `release_image(context, key)`。key 是 Mod 自己的名字
  （`a-z 0-9 . _ -`，≤64 字节），路径是绝对路径的 PNG，**只读一次**；返回绘制用的 handle、尺寸和
  `revision`。同一 key 再加载 = 换像素：**handle 不变、revision +1**，旧 GL 对象延后一帧释放。
* **绘制表 V3**（完整 V2 前缀 + 一个图元）：`image(context, image, quad)`，四角为屏幕像素
  （ImDrawList 顺序）、四组 uv、`tint`；计入每实例每帧 4096 条配额，受 frame 裁剪框约束。
  解析不到 `AddImageQuad` 的构建继续发 V2 表，所以"看到 `draw->version >= 3` 才有 `image`"永远成立。
* **实现**：图片表是纯账本（`src/component_images.hpp`，`tests/component-images.cpp` 离线钉住键规则、
  handle 稳定性、revision、替换/释放时的回收、每 Mod 16 张 / 16 MiB 配额、跨 Mod 句柄不可见）；
  GL 对象仍由 `ui_texture::Manager` 持有，owner = 该 Mod，卸载/停用时随其它纹理一起回收；
  绘制时按"当前回调属于哪个 Mod"在自己的表里解析句柄，插件**永远拿不到 renderer id 或 GL 对象**。
* **`examples/clock` 迁移**：删掉自己的 WIC 解码器、COM 单元、`create_ui_texture` 调用与三个引擎
  入口，改用 `load_image` + `draw->image`；老加载器上退回它本来就有的手绘掉落标记。它的
  `mod.json` 也去掉了不再使用的 `texture` 能力声明。
* **真机证据**（`clock-period-window`，三条新断言）：加载器日志
  `Component render: image "bit-sprite" for local.clock is 200x1100 (revision 1)`、Mod 自己的
  `the game's own bit sprite came from tc.component.render V7`、以及驱动器**用服务替换该图**
  （`revision 2`）后从帧缓冲读回取值标记的像素、断言它变成替换图的品红。同时断言不出现回退路径。
* ABI 基线 1415 → **1471** 条（新增记录全属 `TCComponentRenderDrawV3` / `TCComponentRenderQuadV1` /
  `TCComponentRenderImageV1` / `TCComponentRenderApiV7` 与其常量，现有记录一条未变）。

## 未发布：`tc.component.instances` V2 —— 元件在游戏眼里是哪一个

自定义元件里可以再放一个自定义元件。生成的回调是按**内层实例自己的 64 位 id** 调用的，而玩家点到的、
游戏自己的选中集与撤销记账认的是**外层元件**；做交互的 Mod 必须把这两个 id 对上。这一对是加载器编译
遍历时本来就知道的数据，但此前只以 `interactive-map.txt` 这个**诊断文件**的形式落在游戏目录里——
`examples/clock` 就是往自己插件数据目录上跳三级去读它，任何一次加载器改动换了文件名或位置，那块
交互就会静默失效。

* **新服务版本** `tc.component.instances` V2（完整 V1 前缀 + 一个调用）：
  `int (*top_level_id)(context, handle, uint64_t* out)` —— 游戏选中/顶层遍历认的那个 id；直接放在
  棋盘上的元件就是它自己的 `instance_id`，装在别的自定义元件内部时是外层元件的 id。句柄过期返回
  `ERR_STALE`，只认 V1 的加载器返回 `ERR_UNAVAILABLE`，**两种失败都保证出参为 0**。
* **SDK**：`sdk/tc_component_instances.h` 增加 `ApiV2` / `tableV2` / `topLevelId` / `v1(apiV2)`
  （V1 半视图按字段复制，同一个 `ApiV2` 值可以直接喂给老的五个入口）。
* **实现**：编译遍历时把每个自定义记录的 `+0x18`（顶层 id；棋盘级元件等于自身的 `+0x08`，已在该构建
  实测）记在实例绑定上，服务调用只是一次查表——不读棋盘、不阻塞。那条规则单独放在
  `src/component_container.hpp`，`tests/component-instances.cpp` 离线钉住（含嵌套、两级嵌套、
  容器字段为 0、非自定义记录不参与、同 id 后写的覆盖先写的）。
* **`examples/clock` 迁移**：先问服务，只有加载器没有 V2 时才退回读 `interactive-map.txt`；
  真机用例 `clock-period-window` 继续通过（它点击元件的角标、观察元件自己的输出与选中状态）。
* ABI 基线 1402 → **1415** 条（新增记录全属 `TCComponentInstancesApiV2`，现有记录一条未变）。
* `interactive-map.txt` 从此只是诊断文件（节点顺序不变，值改为"容器为 0 时回落到自身 id"）。

## 未发布：修好两个宿主脚手架（`native-host`、`hook-chain`）

`tc.mod.storage` 给 `NativeRuntime` 加了一个 Mod 存储成员，它需要一个真实的存档根目录；
两个宿主脚手架（`tests/native-host.cpp`、`tests/hook-chain.cpp`）还在传空路径，于是刚进
`NativeRuntime` 就抛 `filesystem error: cannot make absolute path: Invalid argument []`，
host 层的这两条用例一直红着（加载器本身不受影响——`loader.cpp` 传的是游戏自己的 profile 目录）。
现在两个脚手架各自在自己的沙箱目录里建一个 `profile/` 再交给运行时，host 层恢复 4/4。

## 未发布：安装器改成一个窗口（`docs/PLAN-installer-ui.md` 第一刀～第三刀）

旧的安装器是"选 `Turing Complete.exe` → 一个是/否 MessageBox"，玩家看不到它找到了哪份游戏、
为什么拒绝写入，也看不到操作之后的结果。现在它是一个单窗口的安装中心：左边列出检测到的位置，
右边显示所选位置的兼容性、加载器状态和安全检查，只有**一个**主操作按钮。

* **拆成五个翻译单元**（计划 §6）：`src/setup.cpp`（入口与无界面模式）、`src/setup_core.*`
  （inspect / install / uninstall，结构化错误，不弹框）、`src/setup_discovery.*`（Steam 与手动
  位置、规范化、去重）、`src/setup_state.*`（纯状态→动作规则）、`src/setup_window.*`（窗口）。
  核心逻辑一行没改：哈希白名单、`no_links`、原子替换、备份校验、拒绝覆盖全部原样保留，
  `tests/setup.ps1` 原有 6 个用例一次性通过。
* **状态 → 主动作是纯函数**（计划 §4）：`tc::setup::actions_for` 把快照映射成一个主操作、一个
  上下文次操作和 `更多操作` 菜单。不兼容 EXE、未知引擎、备份缺失或损坏这四类状态在任何组合下都
  不会给出安装/修复/升级/卸载；只读目录才出现"使用管理员权限安装"；游戏在运行只会出现
  "关闭游戏后重试"。`tests/setup-state.cpp` 逐条钉住这个矩阵（含"运行中优先于只读""备份问题优先于
  提权"两条优先级）。
* **交互**（计划 §3、§7）：卸载只在 `更多操作` 里，点开是**内联确认区**（"恢复原始引擎，但保留
  Mod、配置和存档"），没有是/否对话框；工作期间禁用位置切换与关闭窗口，并按阶段显示
  "正在再次检查游戏 / 正在备份原始引擎 / 正在安装 Mod Loader / 正在验证安装结果"，不显示百分比；
  成功后不弹 MessageBox，右栏原地变成结果横幅（`[完成] [打开 Mods 文件夹]`）。
* **失败分两层**：首屏一句可行动的中文（来自 `SetupError`），`更多操作 → 查看技术信息`展开路径、
  三个实际哈希、三个期望哈希、兼容配置与进程状态，并可一键复制；同时用操作前后的引擎哈希对比给出
  "游戏文件没有改变：引擎哈希与操作前一致"或"已恢复到操作前的状态"。
* **扫描与写入都在工作线程**：大文件哈希不再让窗口假死，结果通过窗口消息回到 UI 线程。
* **提权**：先做一次只读可写性探测（自删除的临时文件，不碰游戏文件），只有确实不可写时主按钮才变成
  "使用管理员权限安装"，用 `runas` 重新启动同一个 EXE 的无界面模式 `--install "<目录>"`，父窗口等它
  退出后重新扫描并展示真实结果；取消 UAC 显示"未进行任何更改"，不算失败。
* **资源**：`src/setup_resources.rc` + `src/setup-manifest.xml` 提供 Per-Monitor V2 的 DPI manifest、
  `asInvoker` 执行级别和由 `VERSION` 生成的版本块（首次让安装器 EXE 有正确的文件版本），以及
  `assets/tc-modloader.ico`（由 `tools/make-icon.ps1` 生成，7 种尺寸）——窗口、任务栏和资源管理器
  从此有自己的图标，而不是通用程序图标。
* **外观**：默认跟随系统深浅色，高对比度下退回系统颜色；`Segoe UI Variable Text` → `Segoe UI`，
  中文字体回退交给 GDI；Tab 顺序、方向键换位置、Enter 执行主操作、Esc 取消确认/关闭结果。
* **无界面模式保持兼容**：`--install <目录>` / `--uninstall <目录>` 的参数与退出码不变（0 成功、
  1 失败），发布测试与提权路径都走这两个入口；`tests/setup.ps1` 从 6 个用例扩到 10 个，新增
  "备份缺失拦下写入""备份损坏拦下安装与卸载""卸载保留 mods/ 与 tc-modloader-data/""不完整的游戏
  目录被拒绝且不产生副作用"。

## 未发布：Mod 全局配置、schema 迁移与统一设置页

* 新增 `sdk/tc_mod_storage.h` 与服务表 `TCModStorageApiV1`：插件用 key 读写不透明 blob，支持
  计数式读取、原子整值替换、存在检查与删除。
* 两个隔离 scope：GLOBAL 位于调用 Mod 自己的 `plugin-data/<id>/storage/`；PROFILE 位于当前
  独立存档内的 `tc-mod-data/<id>/`，因此切换存档会切换数据，复制存档会一起复制。
* 所有权由 `query_service` 的插件上下文决定，调用方不传 Mod id 或路径；key 只能是最长 64 字节的
  小写安全名称。每项 1 MiB、每 scope 256 项 / 8 MiB，路径经过重解析点检查。
* 离线用例覆盖 Mod 间隔离、scope 隔离、空值、覆盖、删除、非法 key、大小限制和 profile 复制；真机
  用例启动三次并切换 A → B → A，断言全局计数 1/2/3、存档计数 1/1/2。
* `tc.mod.storage` V2 给每个 key 增加宿主管理的 schema 版本与一次性迁移回调。版本和 payload
  同在一个 envelope 里原子替换；回调拒绝/异常/越界/竞态均保留旧值，拒绝降级；V1 仍只看 payload。
* 新增 `tc.mod.settings` V1 与 `sdk/tc_mod_settings.h`：bool/int/float/enum/text 字段由宿主复制、校验，
  统一页面按 Global / Current save 分组，自动保存、默认值和 Reset，可选回调能拒绝改动。
* `local.board-grid` 成为完整消费者：旧 `board-grid.txt` / schema 0 `grid=…` 先迁到 schema 1，
  再把布尔字段交给统一设置页，不再 Hook 游戏自己的 Options 页面。

## 未发布：元件属性行 `tc.component.editor` V1（2026-09-27 第一刀）

计划 §14（声明式属性编辑器）的第一个切片，按用户的"`tc.component.editor` 可以先做"排在最前。
它把"行窗口盖在游戏元件抽屉上、自己收输入、用 `write_config` 提交"这套配方从每个 Mod 手里收进
加载器：**类型交一张字段表，宿主画行、收鼠标键盘、提交**。

* **新服务** `tc.component.editor` V1（`sdk/tc_component_editor.h`）：`set_fields` /
  `clear_fields`，字段种类 bool / int / uint（≤31 位）/ enum / text（2–64 字节）/ float（binary32），
  带 `READ_ONLY` 与 `HEX` 标志。字段是**配置 blob 的字节偏移**，按小端读写，因此不必匹配 C 结构的
  padding；越界、未知种类、宽度不对、枚举没有选项只有整表拒绝，日志写明是哪一条。
* **行为**：行画在 Mod 自己的 `TC_UI_SLOT_BOARD_COMPONENT_PANEL` 之后（没有插槽的类型照样有
  编辑器）；提交走 `storageWriteConfig` 的同一条路，所以**一次提交 = 一步撤销**并触发
  `on_config_changed`；编辑期间宿主保留工作副本，提交后按新的 `config_revision` 重新读取。
  `TC_EDITOR_TRACE=1` 时每秒把每行矩形写进日志，真机用例据此点击。
* **示例**：`examples/editor-demo`（0 引脚、32 字节配置、五种字段各一个，只交表不写界面）。
* **证据**：离线模型单测 `tests/component-editor.cpp` 进 `build.ps1`；ABI 基线 1238 → **1281** 条；
  真机隔离沙箱两条门槛都通过——`tests/component-editor-playtest.ps1`（注册 + 字段表被受限构建
  接受）与 `-Stage rows`（`tests/component-editor-driver.cpp` 进 sandbox、用命令总线放一个
  `EDIT_001`、点元件本体打开游戏自己的抽屉、按宿主自己打出的行矩形点中 `enabled` 复选框；文本行
  再点开字段、打字、回车）。断言落在**元件 Mod 自己的 `on_config_changed`** 上：默认 `enabled=1`
  被点成 `0`，`label` 从 `"hello"` 变成 `"helloxy"`。三条用例进 `tests/test-catalog.json`。
* **踩到的坑（已写进 `docs/HANDOFF-component-editor.md` §5）**：`tcmod-cli apply` 是"设定启用
  集合"而不是追加，两次调用会让第一个 Mod 失效（类型没注册 → 放元件只能返回 `-6`）；
  `PLACE_COMPONENT` 在目标格已被占用时同样返回 `-6`；抽屉在屏幕底部，行窗口要夹进屏幕，否则
  下面的行画得出来但点不到。
* **尚未交付**：下拉/弹窗选择器、颜色字段、可编辑十六进制框、行内校验提示、自定义行绘制，
  以及把多字段改动合成一步撤销（`tc.component.storage` V2 的 `begin_edit`/`commit_edit` 还没被
  编辑器用上）。

### 同日第二刀：`tc.component.editor` V2（布局标志、单选、十六进制、滚动）

* **V2 服务表** `TCComponentEditorApiV2`（与 V1 同样的两个调用）：能查到 V2 就说明加载器认下面
  这些 flag；V1 加载器会忽略它们，所以 Mod 应当先问 V2、拿不到再降级。
* **布局标志**：`FULL_ROW`（控件占整行）、`INLINE`（与上一字段共用一行）、`SEPARATOR`（本行前
  一条分隔线）。表级校验：首行不能 INLINE；`FULL_ROW`/`RADIO` 之后不能 INLINE；最多两列；
  **未知 flag 位拒绝**而不是忽略。
* **枚举单选** `RADIO`：一把选项铺成一行单选按钮，正是 `examples/float-ops` 今天手画的形状。
* **可编辑十六进制** `HEX`：UINT 变成 `0x…` 文本框，宽度上限从 31 位提到 **64 位**；解析不了的
  输入被拒绝、配置保持旧字节并写日志。
* **滚动**：行窗口按内容高度（上一帧窗口高度 + 滚动范围）夹进屏幕，再用尺寸约束封顶在屏幕内，
  装不下的部分由 ImGui 滚动——32 行的类型实测 `rows=32 scrollmax=458`，第一行照样点得动。
* **诊断**：新增 `Component editor: window instance=… rows=… scrollmax=…`，以及画不出行时的
  `skipped (原因)`（实例没绑定 / 类型没声明 / 配置读不出来）。
* **示例**：`examples/editor-demo` 现在有两个类型——7 字段的 `EDIT_001`（覆盖全部字段种类与
  V2 flag）与 32 行的 `EDIT_002`（滚动）。
* **证据**：离线单测（`tests/component-editor.cpp`：表级规则、未知 flag、HEX 校验与解析、
  64 位往返、`formatHex` 补零）；ABI 基线 1281 → **1296** 条；真机六条用例
  （`component-editor`、`-rows`、`-text`、`-radio`、`-hex`、`-long`）全部通过，断言都落在元件
  自己的 `on_config_changed` 上。
* **这一刀踩到的坑**（写进 `docs/HANDOFF-component-editor.md` §6）：夹紧要用内容高度；驱动的
  鼠标点击会抖，重试要在目标点周围补点；游戏的输入框收不到 Home/Delete/Ctrl+A，只有字符能进；
  `test-catalog.json` 的描述必须保持 ASCII。

### 发布流程：安装器与加载器成对校验 + 已发布旧版本的加载器登记（2026-09-28）

出 0.8.1 这一步时把两个**发布期的坑**堵上了：

* **安装器和它部署的加载器过去没有一致性断言。** `build.ps1` 的顺序（先 `dist\tc-loader.dll`
  → 再由 `VERSION` 生成 `build\setup-version.rc` → 再 windres 编译 `src\setup_resources.rc`
  （`101 RCDATA "../dist/tc-loader.dll"`）→ 最后链接安装器）保证了同步，但
  `tools\release.ps1 -UseExistingBuild` 会整段跳过构建，谁也无法保证 `dist` 里那对文件还是配套的
  （文档警告过的"手工编译的 DLL 哈希不同"正是这个坑）。现在 `tools\ReleaseTools.psm1` 多了
  `Assert-TcInstallerMatchesRelease`：从安装器的 PE 资源里抽出 101（RCDATA），与
  `dist\tc-loader.dll` 逐字节比 SHA-256，并断言安装器的 `FileVersion`/`ProductVersion` 等于
  `VERSION`。它被 `tools\release.ps1`（暂存之前）与 fast 层的 `release-contract`
  （`tests\release.ps1`）同时调用，所以"build 一下 + 测试全绿"也会顺手验掉这一对。
* **已发布版本的加载器哈希要登记，否则老玩家升不上来。** 安装器只认它的白名单
  （`compat\profiles.json` 的 `acceptedInstalledLoaders` → `src\compat.generated.hpp` 的宏 →
  `src\setup_core.cpp` 的 `classify()`）；不在名单里的引擎一律读成 `installed_unknown`
  （界面"未知引擎 / 已禁用安装"，不覆盖也不给卸载）。实测：0.6.0 发布包内嵌的加载器
  `f609783c…`（4,487,399 B）与 0.8.0 的 `5702228f…`（5,211,697 B）**都不在名单里**——也就是装了
  0.8.0 的机器无法被 0.8.1 升级。这一版把两条登记进去，并把 `classify()` 改成查表
  （新增 `TC_LOADER_060_SHA` / `TC_LOADER_080_SHA`）。`tests\setup.ps1` 增加"从已发布的加载器
  升级"这一相：把历史发布包内嵌的加载器铺进沙箱当 `game_engine.dll`（旁边放原版引擎当备份），
  要求 `--install` 退出码 0 且部署出来的正是当次的 `dist\tc-loader.dll`——实测
  `PASS upgrade from the published 0.6.0 loader` / `… 0.8.0 loader`。

## 0.8.0（2026-09-27）

### 发布阻断项

* **游戏更新不再让游戏起不来。** 存档重定向（`src/save_boot.hpp`）原先在 `DllMain` 里校验失败
  就 `return FALSE`，而加载器本身就是 `game_engine.dll`——于是"游戏更新"会直接变成"游戏完全无法
  启动，只能先卸载加载器"，连 Shift 安全模式和 `tcmod-cli disable-all` 都救不了（它们都要求加载器
  已经装上）。现在重定向失败只是**不重定向**：
  - 游戏照常启动，玩家自己的存档路径保持原样；
  - 本次运行**不加载任何原生插件**（避免 Mod 写进玩家的真实存档），资源 Mod 照旧生效；
  - `loader.log` 写明原因，并**弹一次**原生对话框（按"加载器版本 + 游戏哈希"记住，每个新组合只提示
    一次，同时写 `tc-modloader-data/unsupported-build.txt` 供排查）。用对话框而不是游戏内页面，是
    因为那种情况下加载器知道的 RVA 已经不可信，游戏内页面可能根本不会被画出来（实测：不匹配时主菜单
    连 Mods 按钮都不会出现）。
  - 判定抽成纯函数 `tc_save_boot::patchable(...)`，由新用例 `tests/save-boot.cpp` 钉住"任何无法验证
    的字面量都拒绝打补丁，而不是让加载失败"。
* **版本与打包**：`VERSION` 升到 **0.8.0**；重新跑发布流程（确定性源码包/玩家包 + `release.json` +
  两层 `SHA256SUMS.txt`）并发布到 GitHub。

### 本版主要能力（0.4.0 之后的累积）

* **元件外观**：`tc.component.render` V5（元件栏卡片 + 抽屉预览图由 Mod 提供）与 V6（用同一个绘制
  回调接管放置幽灵，结束信号取自游戏自己的剪贴板记录生命周期）；
* **元件体系**：声明式 C++ 元件（`register_component`）、原生逻辑的形状/位宽扩展、每实例配置持久化
  （写进元件记录自己的表，随原理图存档）与 schema 迁移、引脚顺序/命名；
* **插件界面**：主菜单页面、嵌进游戏布局的面板插槽、纹理上传、类型化控件与绘制原语；
* **仿真**：状态读取、逐周期捕获、IO 值读写、波形导出、事务与命令总线；
* **存档管理页**（主菜单 `Mods` 之下的「存档」）：副本列表 / 新建 / 导入原版 / 复制 / 重命名 /
  删除到回收站 / 切换 / 打开目录，加 **Mod 依赖清单**（登记表缓存 + 电路扫描，列出"需要哪些 Mod、
  缺哪些、版本或 schema 不符"），以及 2026-09-27 的卡顿修复（不再每帧扫盘/读 ini，扫描改单趟窗口
  匹配并把耗时写进日志）；
* 其它：能力协商与依赖版本约束、事件总线、符号别名与钩子链、安全模式、故障日志。

下面按时间倒序保留每项的细节（0.4.1 起这些条目此前都还没有单独发包）。

## 0.4.1–0.7.x 累积：浮点元件族 M2 与后续调研（2026-09-25 起）

### 存档管理独立成页：主菜单 `Mods` 之下的「存档」（2026-09-26 夜，本轮交付）

* **卡顿修复（2026-09-27，玩家报"感觉有点卡顿"）**：页面最初把那两件事放在了**每帧**做——扫一遍
  所有副本（实战副本 139 个电路文件）、`saves.ini` 读两次（`GetPrivateProfileString` 是文件 I/O）、
  `trash-*` 再扫一遍；选中一行还会重扫该副本并重新解析所有 `.mod` 包。现在：
  - 列表只在**打开页面 / 点「刷新」/ 副本目录本身变了**时重建（每帧只做一次 `last_write_time`），
    实测刷新 139×2 个文件的副本从**每帧**降到**一次 15–47 ms**；
  - 依赖报告**每次会话每个副本只扫一次**（打开页面时只自动扫当前选中的那份，其它行点「扫描」），
    并且不再重扫 Mod 包（用内存里已有的 `core->mods`）；
  - 扫描本身从"27 个 id 各扫一遍全文"改成**单趟 8 字节窗口 + 哈希表命中**，Snappy 解压的 copy
    段也从逐字节 `push_back` 改成按块 `insert`；
  - 每次刷新/扫描的时间都写进 `loader.log`（`Save profiles refreshed: N profiles, X ms`、
    `Dependency scan <id>: N circuits, X ms`），下次再有人说卡可以直接看数字。
  实测（同一台机、同一份 139 电路副本）：报告扫描约 **140 ms**（一次性），页面静止时每帧开销≈0。

存档管理从 Mod 页里那段折叠内容搬出来，成为加载器自己在主菜单上的**第二个入口**（`Mods` 正下方，
插件注册的页面整体下移一格）。页面每次打开都重新扫一遍 profiles 目录，加载器自己不存列表状态，
所以显示的和磁盘上的一致。

* **列表**：每行一个副本——名字、占用大小、文件数、最后修改时间，以及「本次运行 / 下次启动 /
  导入副本」标记；`trash-*` 不计入列表，只在标题里报数量。
* **操作**：新建空副本（`local-<随机>`）、导入原版存档（`import-<随机>`，逐文件校验）、切换到
  这一份（写 `saves.ini`，重启生效）、复制副本（`copy-<随机>`，同样逐文件校验）、重命名（限制
  `a-z 0-9 -`）、删除到回收站（`trash-<原名>-<时间戳>`，当前副本拒绝删除，改回名字即恢复）、
  打开文件夹。
* **本次运行 vs 下次启动**：进程在启动时就把游戏自己的存档路径常量重定向到选定副本，所以页面上
  改的选择只影响下一次启动——和 Mod 的「应用更改」同一套语义，页面上也照这个措辞显示。
* **开发用**：`TC_MODLOADER_OPEN_SAVES=1` 免点击打开这一页（配合 `TC_MODLOADER_SHOT` 出图），
  两个入口的按钮矩形也都进日志（`Saved` 与 `Saves`），供 playtest 点击。
* **Mod 依赖（③，2026-09-27）**：详情区第一行给结论，下面是明细。
  - **电路里只有 id**：每个自定义元件在存档里只留 8 字节 `custom_id`（作者自选的标记，如
    `F32ABS_1`），没有 mod id。所以加载器在**元件注册时**把 `id → {Mod id、类型名、config schema、
    包 SHA、注册日期}` 记进 `tc-modloader-data/registry.json`——这是"Mod 卸掉之后仍能说出这是谁的
    类型"的唯一来源。
  - **扫描**：逐个解压副本里的 `circuit.data`（首字节版本 + 原始 Snappy 块，`src/snappy_raw.hpp`），
    先精确搜登记表里已知的 id，再按"8 个可打印字节、且要么后面 32 字节内出现加载器自己的 `TCM3`
    配置标记、要么同一文件里出现两次"挑出未知类型标记。第二条是启发式（玩家存档是格式 v15/v16，
    组件记录无法逐字段走），报告里如实标注。
  - **结论**：装好并启用绿、已装未启用黄、缺失红、版本不同单独标（对照登记时记下的版本）；
    未知类型只显示作者标记。`tcmod-cli <游戏目录> saves report [副本]` 输出同一份数据，
    用例 `save-isolation` 用手工构造的 Snappy 电路 + registry 断言了这两条路径。

### 放置幽灵绘制接管：`tc.component.render` V6（2026-09-26，本轮交付）

* **SDK**：新增 `TC_COMPONENT_RENDER_API_VERSION_6` / `TCComponentRenderApiV6` 与
  `tableV6()` / `setPlacementPreview()`。按类型启用后，已有绘制回调会收到 `instance_id == 0`、零组件
  句柄和空配置的预览帧，Mod 用类型默认值画本体、文字和引脚。
* **加载器**：接管 `redraw_clipboard_component`，从游戏自己的临时记录读取 custom id、吸附格点与旋转，
  抑制默认的“仅引脚/名字”幽灵；捕获结果每帧重复提交，避免即时绘制只闪一帧。结束放置的信号取自
  **游戏自己的剪贴板记录生命周期**：`update_state_clipboard__…u120` 每次剪贴板状态变化都被调用，并拿到
  它正在处理的记录——kind `0x4e` + 同一个 custom id 表示这次放置还活着，空记录或换了一个元件表示游戏
  已经把剪贴板放下（落下、取消、换成别的选择）。`hide_clipboard` 钩子保留为兜底。
* **为什么不是别的信号**：`is_clipboard_visible__…u2911` 看着像“幽灵是否可见”，实测却不是——按住元件
  拖动的整个过程里它是 **0**（游戏自己都走“不可见”分支；接受这条信号会让预览一帧都不画），而红绘路径
  只在游戏继续重绘时才被调用，成功放置后它不再调用、`hide_clipboard` 也不调用，于是只挂这两个钩子的
  版本把幽灵留在落点：玩家报的第一个 V6 bug（`Body=424`，证据见
  [verification.md](verification.md) 的「元件放置幽灵 V6」）。
* **Float Ops**：22 种类型全部选择 V6，继续复用棋盘上的同一套紫色外观；预览哨兵 id 不进入实例命中缓存。
* **真机验收**：`tests/picture-playtest.ps1` 从“幽灵无本体”的旧断言改为检测拖放点附近的紫色本体和红色
  引脚。连续四帧都出现完整 `ABS |x|`，无论 V5 图片注册开关如何；幽灵区域的 V5 品红标记始终为 0。
  松开鼠标后再拍一帧（`stage after`）断言落点附近紫色为 0；往棋盘上**真的放一个元件**再拍
  `stage placed`/`stage settled`，断言元件确实落在落点上、被取消的那次拖放留下的格子仍然干净，并用
  加载器自己的 `taken over`/`ended` 计数断言每一次接管都结束。用只挂 `hide_clipboard` 的上一版加载器
  跑同一脚本，两条运行都在 `after` 一帧留下 `Body=424`（失败）。
  探针同时修正了浮点目录按钮边界点击、抽屉图片误覆盖元件栏取件坐标，以及放置落点读回系统光标这三处
  偶发项。

### 元件栏卡片与抽屉预览的图片接管：`tc.component.render` V5（2026-09-26，本轮交付）

玩家要的是"元件在元件栏里就长成棋盘上的样子"。上一轮把机制测清楚了（研究文档
`docs/research/component-icons.md`：图片是游戏自己渲染的，往游戏请求的路径上放文件没用，必须接住
那次**纹理请求**），这一轮把它做成能力并接到真机上：

* **SDK**：`TC_COMPONENT_RENDER_API_VERSION_5`/`TCComponentRenderApiV5` 的
  `set_picture(context, custom_id, png_path)`（`sdk/tc_service_api.h`），
  `sdk/tc_component_render.h` 加 `tableV5()`/`setPicture()`。路径必须是绝对路径；传 `NULL`、
  或者游戏真要这张图时文件已经不在了，就回退到游戏自己的渲染。
* **加载器**：`queryComponentRenderService` 增加 V5 分支（`component_render_set_picture_api`，按
  `componentIds` 校验归属，和 `set_foundry_button` 同一套写法），并新增 `armPictureControl()`：
  按住游戏纹理工厂 `create_texture_unsafe__…u364` 与它的检查版包装 `create_texture__…u1978`。
  命中 `?snapshot_cc/com_custom_<十进制 id>.png` 且该 id 有图时，用**加载器自己那份副本**的路径去调
  原工厂，其余请求（内置 `?snapshot/<kind>.png`、精灵、菜单、字体、游戏自己的缓存）原样转发。
  副本放在 `<game>/tc-modloader-data/pictures/<id>.png`：游戏读完会把重编码结果写回**它读的那个
  路径**，指给 Mod 包里的文件会让加载器的文件台账对不上哈希。规则本身抽成 `src/picture_path.hpp`
  （离线用例 `tests/picture-path.cpp` 覆盖正例与 `?snapshot/…`、缺数字、多后缀、超 64 位等近似反例）。
* **Mod 侧**（`examples/float-ops`）：**用元件外观当缩略图**——新增 `examples/float-ops/icons.cpp`，
  它 `#include "components.cpp"`，把**同一套绘制代码与同一张表**跑在一张"会画像素的绘制表"上
  （GDI+ 画本体与文字、存 PNG），生成 22 张 192×192 的透明 PNG：紫色本体、`32`
  角标、名字、中间符号与引脚都在棋盘那套位置上（一格 26 px）。构建脚本把它们随包放进
  `files/asset/float-ops-icons/<十进制 id>.png`，插件加载时逐个 `set_picture`。
* **真机验收**（`tests/picture-playtest.ps1`，用例 id `component-picture`）：卡片与抽屉预览两面在
  "注册了标记图"的一次里出现标记色、在"关掉注册"的对照里一处都没有；离线用例
  `tests/float-icons.ps1` 直接检查随包那 22 张图（透明画布、本体紫色宽 128 px、引脚触到 ±3.2 格、
  有白字），把"图是元件外观而不是占位图"钉在数据上。
* **要看清楚的一件事**：V5 的真实覆盖面是**元件栏卡片 + 底部抽屉预览**两个面。实测（随包真图、
  三种尺寸的标记图、以及完全不注册）游戏默认幽灵都只有引脚、名字标签和连线预览；这条历史测量后来
  促成了上面的 V6 专用接管。研究文档 §5 同时记了两个图片路径的坑（游戏重写它读过的 PNG、Nim
  字符串必须活过调用）。

### 研究：自定义元件的"图标"从哪来、能不能接管（2026-09-26，本轮实测）

这一轮没有改行为，只把"接管游戏自定义元件的图标绘制"这件事测清楚（完整证据与复现步骤见
`docs/research/component-icons.md`；探针 `tests/icon-probe.cpp`，用例目录 id `component-icon-research`）：

* **那张小图是游戏自己渲染的**。元件栏每一项先向渲染器要一张纹理：
  `get_captured_path__presenterZio_u28` 按 kind 分派（`0x4e` = 自定义原型 → `u25`，其余 → `u15`），
  自定义原型拼出的路径是 `?snapshot_cc/com_custom_<十进制 id>.png`（内置元件是
  `?snapshot/<kind 名>.png`），`get_asset_path` 再把它落到
  `<游戏目录>/asset/?snapshot_cc/com_custom_<id>.png`；请求行逐帧出现（FP32 Constant =
  `com_custom_5058442071141929777.png`）。
* **放 PNG 不能接管**：把一张品红色 64×64 PNG 放到那条路径上（`asset/snapshot_cc/`，另有
  `asset/capture/` 的绿色作对照），游戏**确实读了它**（运行后被重新编码 1060 → 642 字节，像素不变），
  但"放文件"与"完全不放文件"两次运行在元件栏区域逐像素比较 **0 差异**——画出来的是游戏自己的
  设计图缩略图渲染（`render_component_snapshots__main_u414`、`update_dirty_snapshots__main_u873`、
  `is_snapshot_mode__presenterZrendererZmulti95meshZmulti95mesh_u270`）。
* **`shape_svg` 也不能**（对自定义原型）。内置元件的目录缩略图来自原型 `+0xb0` 的 `shape_svg`
  （探针 dump：`<path d="M -12 -18 L -12 18 L 33 0 Z" class="component"/>`、
  `<rect x="-50" y="-30" width="100" height="60" class="component" />`，坐标是本体局部坐标），
  加载器也已把它转发给游戏（`src/native_component.hpp` 的 `setPrototypeShapeSvg`），但给本 Mod 的
  常量加上特征 SVG 后元件栏同样是 0 差异——与 `docs/PLAN-custom-components.md` 早先记下的
  "`shape_svg` 无影响"一致；`set_default_drawing` 只关棋盘上的默认绘制，不关图标。
* **结论**：接管必须在渲染层加能力，两条待做的路线写在研究文档里（A：拦
  `create_texture__u1978`/`create_texture_unsafe__u364` 对 `?snapshot_cc/com_custom_<本 Mod id>.png`
  的请求，返回 Mod 自建纹理；B：接管 `component_snapshot` 的快照渲染）。
* 追加测量（同一天，玩家要求"三个面都要"）：设计图缓冲区 `+0x570` 那条 Nim seq 的**安全写法**已经验证
  可行——`getCustomPrototype` 克隆 → 只写 `buffer+8` 起的 512 字节格子（8 字节 seq 头不动，
  上一轮崩溃就是涂了它）→ `setCustomPrototype` 回写 → `releasePrototype`，游戏不崩、**棋盘画面确实
  跟着变**（第 9 帧棋盘区 244 个采样点不同）；但**元件栏卡片区 0 像素差异**，所以"只改设计图"覆盖不了
  三个面。后续逐面复测修正了当时的假设：只有元件栏卡片与抽屉预览消费
  `?snapshot_cc/com_custom_<id>.png`；放置幽灵走独立 clipboard redraw 路径，现由 V6 接管。

顺带两条操作经验记进了文档：元件栏是**悬停逐层展开**的（分类页签 → 子分类飞出层 → 元件飞出层），
而且 1200×800 的小窗口下游戏**不画**元件列表，所以该用例默认用玩家自己的窗口尺寸 2536×1452。

### 修：退出关卡再重进后常量回到默认值、底部面板不再回读配置（2026-09-26，玩家第六次反馈）

玩家这三句话是一串同一件事："1. 常量的值还得我点一下才会刷新；2. 进入沙盒后退出再重进，会发现选中元件之后
面板的相关配置不会渲染；3. 同 2 重进的时候，常量值输出变为默认"。用隔离存档 + 真机驱动
`tests/float-reentry-driver.cpp`（进关卡 → 点常量本体 → 点"常量值"输入框 → 输入 → 回车 → Esc 退出 →
再按关卡入口 → **只选元件，不碰任何输入框**）走完一遍，日志给出三个各自独立的根因：

1. **加载器的电路存档写错了目录**（本轮最关键的一个）。`TC_COMMAND_SAVE` 的 `saveCurrentCircuit` 用
   **关卡名**拼路径：`schematics/<关卡名>/Default/circuit.data`；游戏自己读写的是关卡 meta 里的
   **kind 目录**（`campaign/<关卡名>/meta.txt` 的 `kind = …`）。关卡 "The Sandbox"（目录
   `campaign/sandbox`）的 kind 是 `architecture`，它的电路一直在
   `schematics/architecture/Default/circuit.data`；于是 Mod 改好的配置被写到
   `schematics/sandbox/Default/circuit.data`——一个游戏永远不读的目录——退出再进自然读到旧文件，
   常量就"变回默认值"。测量方式：把两份不同的夹具分别放进两个候选目录，游戏只会加载 kind 的那一份
   （两次真机测量，另一份 1887 字节的目录夹具完全没被碰过）。修法：新增 `levelSchematicKind()` 读关卡
   meta 的 kind，存档路径改用 kind 目录，日志写明
   `(level "sandbox" keeps its board in the "architecture" schematic the level's own meta names as its kind)`；
   没有 meta 的关卡仍退回原行为。
2. **配置恢复与棋盘刷新必须由关卡生命周期驱动，不能等玩家点面板**。加载器现在在 `level.load` 就记住
   仿真模型（`tc::sim_control::Store::remember`），并在每一帧给"已绑定但还没探到尾部记录"的实例补做一次
   配置恢复（`tc::logic::restorePendingTailConfigs`）。否则暂停中的棋盘在这两个时刻根本没有可用的仿真模型，
   Mod 请求的刷新只会拿到 `ERR_STATE(-5)`（日志行 `float-ops: the pending board refresh expired (status -5)`），
   玩家看到的就是"必须点一下才刷新"，重进时就是"常量还是默认值"。
3. **面板的行只跟着"实例 id"走**。重进关卡后元件 id 不变（游戏按记录发 id），底部抽屉于是继续显示旧棋盘
   的内容，甚至不回读记录。Mod 侧改成：每帧看一次 Board 句柄，换棋盘就丢掉编辑器状态、渲染盒子与值缓存
   （`serviceBoardChange`，日志 `float-ops: board entered (handle 3/2) …`；离开时
   `float-ops: no board is up; the editor rows and the board caches were dropped`）；面板每次绘制都比对
   "行是按哪份记录填的"（`readConfigSnapshot`/`recordMatchesRows`），记录变了（恢复、撤销、克隆、换关卡）
   就重读，正在编辑的输入框不打断；渲染盒还没画过时类型改问宿主的实例表（`typeOfLiveInstance`），
   所以刚进关卡也能立刻显示配置。新增证据行
   `float-ops: the drawer is editing instance 0x… (type 0x…, label "…", value "12.5")`：面板里的值第一次
   可以直接从日志读出来。

真机门禁：`tests/float-reentry-playtest.ps1`（目录 id `float-reentry-game`）。断言写入到达实例记录、电路保存
成功、第二次进入时**没有任何点击**就先恢复出上一次的值（显示文本与位模式与第一访问结尾一致）、抽屉确实重画
并回读了记录（面板文件帧号更大、`value` 行在、字段文本等于保存值）。**红绿都实测**：用上一轮的 loader+Mod
（`-Loader` / `-Mod` 指向备份）跑，编辑被拒、没有电路保存、抽屉不回读 → 用例失败；换这一轮的构建 → 通过。
相邻用例（面板输入 `float-panel-game`、M2 垂直切片 `float-m2-game`、目录 19 型 `float-catalogue-game`）
复跑通过。

顺带两处与本次调查直接相关的收尾：`abi/windows-x64.json` 补上本轮 SDK 里
`TCUiSlotDefinition`（组件抽屉槽新增回调 → 48 → 56 字节）的记录，`sdk-abi` 快速用例由红转绿；
`docs/sdk/simulation.md`、`docs/sdk/commands.md`、`docs/verification.md` 里"电路路径按关卡名"
的说法按上面的真机测量更正为"按关卡 kind"。

### 修：四个多脚元件的占位与本体不重合（2026-09-26，玩家第五次反馈 + 加载器 geometry V3）

玩家反馈："其它元件都好了这四个还是有点问题"，指的是 FMA（3 入 2 出）、CMP（2 入 4 出）、
SPLIT（1 入 3 出）、MAKE（3 入 1 出）。量图 + 真机测出来这两件事：

- **游戏把生成的引脚从第 0 行往下排**（入口脚按自己的数量居中，出口脚从 0 开始），所以一边引脚更多的
  元件，本体落在自己那格的**下方**：CMP 的本体覆盖 -0.33…3.33 格（4 输出跨 0…3 行），SPLIT 是
  -0.6…2.6（3 输出跨 0…2）。而 `set_footprint(半宽, 半高)` **只能声明居中的框**，宿主又把半高
  **向外取整**，于是要么在本体上方白留一整格、要么漏掉本体下沿——两个症状玩家都看得见：元件之间留缝，
  以及**可拖区域悬在本体上方的空白里**（矩形就是游戏命中/拖动/占位用的框）。
- **本 Mod 之前按"引脚跨度 + 0.6 余量"长高本体**，2.93 格的面被撑成 3.2/4.17 格，而整格占位只能
  取整到 4/5 格——比本体高 0.8 格，正是玩家看到的缝。

修法分两层：

1. **加载器新增 `tc.component.geometry` V3**：V3 保留 V1/V2 完整前缀，加一条
   `set_footprint_cells(custom_id, x, y, width, height)`，按**整格**声明矩形，宿主**原样存储、
   不取整**（`sdk/tc_service_api.h`、`sdk/tc_component_geometry.h`、`src/native.hpp` 的
   `setComponentFootprintCells`）。给带偏移的本体一条正当通路，而不是继续用居中形式凑。
2. **元件侧改成本体与占位一起算**：本体只在需要时才长高（两侧都能塞进原版 2.93 格面时保持原版），
   并且**落在整格上**；占位框取"本体覆盖的格 ∪ 引脚所在的格"，用 V3 按格声明。结果：

   ```text
   measure float-ops subtract  vertical pitch=3 cell(s)   (2 入 2 出，原版面)  水平 7
   measure float-ops constant  vertical pitch=3 cell(s)   (M2)                水平 6
   measure float-ops fma       vertical pitch=3 cell(s)   ← 之前 4
   measure float-ops split bits vertical pitch=3 cell(s)  ← 之前 4
   measure float-ops make bits vertical pitch=3 cell(s)   ← 之前 4
   measure float-ops compare   vertical pitch=4 cell(s)   ← 之前 5（4 输出跨 0…3 行，本来就得多一格）
   ```

   四个元件里三个和别的元件一样能贴住，CMP 因为一边四根脚占 4 格（这也是最小可能值）。同时它们的
   可拖区域第一次和画出来的本体精确重合（CMP 的本体从 -0.33 到 3.33 格，占位框就是 0…3 格）。
   真机证据与断言在 `tests/float-pitch-playtest.ps1`（`float-pitch-game`）；加载器的
   `component_geometry` 单测加了 V3 前缀与带偏移矩形的往返，几何服务真机烟测（V1/V2）复跑通过，
   目录/面板/元件栏/拥挤棋盘/玩家存档五条真机用例也全部复跑通过。

### 修：垂直方向的占位比本体大，元件贴不到一起（2026-09-26，玩家第四次反馈）

玩家反馈："这些元件在垂直方向上 footprint 好像有点大了，比外观要大一点，导致现在这些元件相互之间
不能贴在一起，而是中间有间隔"，并附了一张自己排的 22 个元件截图。量图 + 真机实测：

- 从截图量：本体宽 129 px = 4.92 格（1 格 ≈ 26.2 px），本体高 76 px = 2.9 格，但每一行的
  中心间距是 103–110 px = **3.9–4.2 格**——即"占位比外观高一格"。
- 真机测地点间距的新探针 `tests/float-pitch-probe.cpp`（用棋盘自己的放置命令，把参考件放在某格、
  再逐格试第二件，每次测量单独占一条带，只记最小的可通过偏移）：**修复前** FP32 Subtract（两边各
  两脚、面 2.93 格高）垂直最小间距 = **4 格**，而同样面高的三个 M2 元件（Constant/Add/Display）
  是 **3 格**。
- 根因：`tc.component.geometry` 的半高是**向外取整**成整格的（`component_geometry::fromHalfExtents`
  的 `ceil(2 * halfHeight)`）。M2 三个元件声明固定 1.5 → 3 格；目录元件却按"本体半高 + 0.1 余量"
  声明：2.93 格的面 → 1.465 + 0.1 = 1.565 → `ceil(3.13)` = **4 格**。于是同样大小的面，目录元件
  比 M2 元件多占一格，元件之间就留出可见的空隙。
- 修法（`examples/float-ops/components.cpp`）：目录元件改为声明"本体自己的半高"（不再加 0.1），
  并与 1.5 的下限取大。于是 2.93 格的面 → 3 格（和 M2、和游戏自带的同尺寸元件一致），
  3.17 格的 Split Bits 仍是 4 格，4.17 格的 Compare 仍是 5 格（它们本来就更高，不能挤）。
- 验收：新用例 `tests/float-pitch-playtest.ps1`（目录 id `float-pitch-game`）跑真机并把间距写成断言：

  ```text
  measure float-ops subtract vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops subtract horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops compare vertical pitch=5 cell(s)  ADJACENT-OK
  measure float-ops compare horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops constant vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops constant horizontal pitch=6 cell(s)  ADJACENT-OK
  ```

  水平方向不变：5 格本体 + 每根伸到框外的脚各占 1 格（两侧都有脚的 7 格，只有输出脚的 6 格），
  这是游戏自己的规则（脚也算占位），游戏自带同尺寸元件同理。同类用例 `-Fixture crowd`、目录、
  面板、元件栏文件夹全部复跑通过。

### 修："所有能写配置的元件都写入不了"（2026-09-26，玩家第三次反馈）

玩家反馈："现在所有能写配置的元件都写入不了，都是写入元件配置失败"，并附上底部抽屉里
"显示方式"一行 + 下面一行灰字"写入元件配置失败"的截图。**根因不在本轮的分类改动**：

- 复现：把玩家自己的存档棋盘（`%APPDATA%\Turing Complete Mods\profiles\default\schematics\
  architecture\Default\circuit.data`，22 个原生元件）当作夹具跑 `tests/float-panel-playtest.ps1
  -Board <该文件>`，驱动照旧点输入框、打字、回车——日志里标签提交是 `(refused)`，
  说明写入通路整条没进去；
- 根因：宿主的 `tc.component.instances` 枚举是**全有或全无**——只有"全部实例都装进了调用方给的
  缓冲区"才返回 `TC_COMPONENT_INSTANCES_OK`，否则返回 `ERR_RANGE`（`src/native_logic.hpp` 的
  `instanceEnumerate`）。本 Mod 之前用固定 8 个 handle 的栈数组，一旦棋盘上的原生元件超过 8 个，
  枚举永远返回 `ERR_RANGE`，Mod 把它当成"这个实例不在棋盘上"，于是**每个**配置写入都失败；
  玩家棋盘从 4 个元件长到 22 个之后必然全挂（沙箱夹具只有 4 个实例，所以一直没暴露）。
  实测还发现实例集合会在编译绑定过程中**继续变大**：按第一次拿到的总数再枚举一次仍可能
  `ERR_RANGE`，所以要循环重试。
- 修法（`examples/float-ops/components.cpp`）：`instanceHandles()` 从 16 个 handle 起步，
  `ERR_RANGE` 时按宿主报的总数 +1 扩容重试（最多 5 次，和 `examples/clock` 同一个套路但多一个
  循环，因为集合会增长）；`writeConfig`/`currentConfig` 都走它，读不到实例时还会按 custom id
  再全表扫一次。失败路径现在**带原因**进日志（`no live handle for instance …` /
  `this write was not attempted because the host reports N instance(s) …` /
  `the host refused the configuration for instance …: <SDK 错误文本>`），下次不用再靠猜。
- 真机门禁：新增 `tests/float-fixture.cpp --crowd`（18 个不接线的 FP32 Display + 后面才是 M2 链，
  共 22 个原生元件）与用例 `tests/float-panel-playtest.ps1 -Fixture crowd`（目录 id
  `float-busy-board-game`）。**已实测这条用例能抓住这个 bug**：把 `instanceHandles` 改回固定 8 个
  handle、不重试，用例就在"`label "xy" (written)` 缺失"上失败；改回来后通过。玩家自己的那块
  22 元件棋盘也用它复现并确认修好（`-Board <玩家存档>` → 同一套断言通过）。

### 浮点元件真的进了游戏自己的分类体系（2026-09-26，路线 2）

玩家否掉了上一轮的"路线 1"（Mod 自己画一个"浮点元件"侧栏面板）："我希望分类文件夹是真的插进
游戏自己的分类体系"。这一轮把分类做进**游戏自己的元件菜单树**，Mod 不再画任何元件栏：

- **机制（实测，不是猜的）**：右侧那一列由游戏自己的 mini tree 绘制，树的来源是
  `get_component_menu__presenterZutilities_u9607` → `reload_component_menu__presenterZutilities_u14192`
  写进 presenter context 的 `+0xd948/+0xd950`。树里每个节点的**第 2 个字节低三位**就是变体标记
  （`2` = 分类、`1` = 自定义元件、`0` = 内置元件），分类节点的 `+8/+0x10` 是名字、`+0x20/+0x28`
  是子节点序列；而给自定义原型**分文件夹的代码就在游戏自己身上**：
  `add_to_menu_tree__presenterZutilities_u12208` 把原型的**名字按 `/` 切开**，逐段当成父分类走进树
  （这正是说明书里"文件夹结构决定自定义元件在菜单中的层级位置"的实现）。
- **做法**：Mod 注册元件时把名字写成 `浮点/FP32 Add`（`components.cpp` 的 `namePrefix`），于是
  **游戏自己**在"自定义"里建出一个 `浮点` 分类节点，并把 22 个浮点元件挂进它——分类节点是游戏
  分配、游戏绘制、游戏销毁的，Mod 不碰任何游戏内存，玩家存档也不受影响。`TC_FLOATOPS_PALETTE`
  可以覆盖这个前缀（自动化用例用它对比折叠/平铺两种菜单）。
- **删掉的东西**：`registerPalette` / `paletteDraw` / `placeType` / `freeSpot` / `palette-rows.txt`
  以及"点一下放到棋盘"的整条路线 1 通路全部删除（约 350 行），Mod 现在只有游戏自己的元件栏和
  游戏自己的底部抽屉（编辑行）。
- **验收**：`tests/float-palette-playtest.ps1` 重写——`tests/float-menu-probe.cpp` 钩住游戏自己的
  `reload_component_menu__presenterZutilities_u14192`，等它建完菜单后把整棵树（供 `build/menu.txt`）
  dump 出来，用例断言：游戏原有五个分类还在、`自定义` 里有一个 `浮点` 分类节点、这个节点的子树
  **恰好**是本 Mod 的 27 个元件 id（22 个可用元件 + 5 个 M0 探针）、而且这些 id 在树的其它任何
  地方都不出现（不是"既在文件夹里又散在自定义"）。

实测 dump（`build/menu.txt`，真机、真包）：

```text
node=… byte1=2 tag=2 name="自定义"
  children=1 payload=…
  node=… byte0=171 byte1=2 tag=2 name="浮点"
    children=23 payload=…
    node=… byte0=171 byte1=2 tag=2 name="M0"
      children=5 payload=…
      node=… byte0=0 byte1=1 tag=1 id=0x4633325041535331 name="浮点/M0/FP32 M0 Pass"
      …（五个 M0 兼容性探针）
    node=… byte0=0 byte1=1 tag=1 id=0x4633324144445f31 name="浮点/FP32 Add"
    …（22 个可用元件）
```

"自定义"页现在只剩这一个 `浮点` 文件夹，里面是 22 个可用元件加一个 `M0` 子文件夹（五个兼容性
探针——它们也是本 Mod 的元件，注册名同样带前缀，所以不再散在自定义根部）。

**已知边界**：`浮点` 文件夹在"自定义"分类页**里面**，不是和布尔/整型/杂项/输入输出并列的第六个
顶级页——钉住的这版游戏把自己的顶级分类写在数据里（本地化 + 分类枚举），而给自定义原型分层的
唯一通道就是上面那个"名字按 `/` 切开"的路径，它固定从"自定义"节点起步。要做成顶级页只能钩住
`reload_component_menu` 之后**动手改游戏自己的菜单树**（拿 `nimNewObj` 造节点、把节点从
`自定义` 的子序列搬到顶级序列），那是在玩家进程里改游戏内存，本轮没做，理由与可行的步骤记在
`docs/research/palette-categories.md`。

### 浮点元件有了自己的元件栏（2026-09-26，路线 1）

玩家要求："把浮点数 mod 的这些元件在游戏内右侧元件栏单独开一个文件夹……而不是都放到自定义元件
文件夹。" 本轮先把结构测清楚（`docs/research/palette-categories.md`，探针
`tests/palette-probe.cpp` + `tests/palette-probe-playtest.ps1`，真机只读，dump 在
`build/palette.txt`）：

- 右侧那一列是游戏**写死的分类页**（布尔/整型/杂项/输入输出/自定义），名字来自游戏自己的
  本地化条目（`# Component category name, at most 6 glyphs…`），不是从存档目录读出来的；
- 原型里 `+0x40` 的"类别"字节并不对应分类页（`NAND` 与 `Multiply` 同为 `3` 却分属两页），
  `+0x88/+0x90/+0x98` 是菜单排序键，`CATEGORY_ORDER` 是 `.rdata` 里 10 个 64 位条目；
- Mod 元件是加载器导入的原生 custom 原型（kind `0x4e`），没有对应的 schematic 文件，因此和
  玩家自制元件一样落在"自定义"页根部；游戏给玩家自制元件分层的机制是**工坊关卡的文件夹结构**
  （说明书原文：文件夹结构决定自定义元件在菜单中的层级位置）。

玩家随后要求"完成后自动关机"，并让按推荐路线做，于是这一轮交付了**路线 1**：

- **Mod 自己的"浮点元件"面板**（`examples/float-ops/components_ui.cpp` 的 `registerPalette` /
  `paletteDraw`）：用加载器已有的**电路板侧栏插槽**（`TC_UI_SLOT_BOARD_SIDE`，与 board-panel
  示例同一个容器，实测该容器里的控件真的收得到鼠标）列出本 Mod 的 22 个元件，一行一个按钮。
- **点一下就放到棋盘上**：点击行 → `TC_COMMAND_BOARD_PLACE_COMPONENT`（加载器的命令总线，也就是
  游戏自己的放置通路，Ctrl+Z 能撤销）。落点由 Mod 自己找空位：先读棋盘上现有元件的位置（记录里
  `+2/+4` 的格子坐标），取平均位置作锚点，再用环形搜索找一个**留出本体占地**的空格——实测紧挨着
  已有元件放会被游戏拒（命令结果 `-6`），所以搜索按 ±6/±3 格的净空判断。
- **自带证据**：放置请求提交后 Mod 会读回命令状态并把棋盘上真的多出来的实例写进日志
  （`float-ops palette: placement command state=3 result=0`、
  `float-ops palette: placement confirmed type=0x… at (-4,5)`）。
- **验收**：`tests/float-palette-playtest.ps1`（真机、真包）：面板注册并画出（加载器日志
  `Board panel local.float-ops/palette …`）、列出 22 行、放置成功且棋盘上确认到新实例。**面板按钮
  的真实鼠标点击**由既有用例 `tests/ui-board-panel-playtest.ps1` 覆盖（同一个侧栏容器里的按钮，
  真实鼠标消息），本次面板用的就是那条通路。

仍未做（写进 `docs/research/palette-categories.md`）：把分类**插进游戏自己那 5 个分类页**（需要钩
`build__…Zmini95tree` / `context+0xd948` 的入口序列并复刻游戏自己的缩略图与拖放），以及按游戏原生
方式把元件写成玩家存档里的工坊 schematic（`PLAN-float-components.md` §8.3 早已否决）。

### M3 + M4：把二进制32的元件目录做完（2026-09-26）

玩家要求："把浮点数mod完整做完，也就是完成剩下的各个阶段。元件外观依旧和目前几个元件保持一样的
格式，如果有属性选择的元件也是用底部面板。" 交付如下：

- **19 个新元件**（`examples/float-ops/components.cpp` 的 `kCatalogue` 一张表驱动注册、绘制与面板）：
  - M3 基础包：`FP32 Subtract` / `Multiply` / `Divide` / `Square Root` / `Negate` / `Absolute` /
    `Compare` / `Classify`；
  - M4 扩展：`FP32 Fused Multiply Add`（三输入单次舍入）、`FP32 Remainder`、
    `FP32 Round To Integral`、`FP32 MinimumNumber`、`FP32 MaximumNumber`、
    `I32 To FP32` / `U32 To FP32` / `FP32 To I32` / `FP32 To U32`、
    `FP32 Split Bits` / `FP32 Make Bits`；
  - 命名统一 `FP32 …`，面板与棋盘上的短名是 `SUB/MUL/DIV/SQRT/NEG/ABS/CMP/CLS/FMA/REM/RND/MIN/MAX/
    I2F/U2F/F2I/F2U/SPLIT/MAKE`。
- **外观与现有元件完全同格式**：本体仍是原版 4.92 格宽、左上 `32` 位宽框、名字右上角、操作符居中、
  引脚在 3.0 通道（本体外 0.54 格）。只有一列引脚超过两行时才长高（Compare 有 4 个输出、Split 有
  3 个），并按引脚中线居中——否则最外的引脚会悬在本体外面。`docs/sdk/ui.md` 与 changelog 同款记录。
- **有属性的元件全在底部面板改**：受舍入模式影响的 10 个元件（除/加/减/乘/开方/FMA/取整/两个整数
  输入转换/两个整数输出转换）在面板里有五个 `RNE/RNA/RTZ/RDN/RUP` 单选框，点了立刻写回该实例的配置
  并刷新棋盘；不受影响的元件（Negate/Absolute/Compare/Classify/Remainder/Min/Max/Split/Make）显示
  一行说明（位序、饱和策略、引脚含义），不做无效的舍入配置。**没有任何弹出窗口。**
- **转换语义按 D2 拍板**：NaN/±Inf/越界置 NV 并饱和（`INT32_MAX`/`INT32_MIN`/`UINT32_MAX`/`0`），
  `-0` 转 0 不置 NV，负的非零值转无符号按越界处理；`i32_to_f32`/`ui32_to_f32` 走实例的舍入模式，
  不能精确表示时置 NX。内核新增 `fp32_from_i32/from_u32/to_i32/to_u32`，SoftFloat 子集随之扩到
  25 个编译单元（`fp/softfloat-sources.txt`，由 `.softfloat-work/closure.py` 重新走符号闭包得到）。
- **验收**（见 `docs/verification.md` 的 M3/M4 小节）：
  - 离线：内核 7571 条黄金向量 + 新增转换策略用例全过；`tests/float-compat.cpp` 断言 19 个新元件
    各自的引脚、宽度、共享配置布局（schema 3）、3.0 引脚通道、callback 的实际输出与 schema-2 迁移；
  - 真机：`tests/float-catalogue-playtest.ps1` 用夹具（19 个元件各一个实例，`float-fixture.exe
    --catalogue`）在真游戏里断言每个元件都被画出来、本体宽度仍是原版 4.92 格（多引脚类型更高）、
    并读出面板行（受舍入影响的类型出现 `rounding` 行、Compare 出现 `info` 行）。

### 面板里的输入框真的能用了；弹出的小编辑器取消（2026-09-26）

玩家反馈与要求（原话）："浮点数元件选中后底部面板的输入框、勾选框用不了"、"这个多出来的额外框我
不需要"。两件事一起解决：

- **根因**（真机实测，`tests/float-panel-playtest.ps1` 逐帧日志）：游戏自己的底部抽屉窗口**不会**
  把鼠标交给插件画在它里面的 item——鼠标停在插件画的那一行上时，抽屉自己报
  `IsWindowHovered()==0`（同一位置的 `panelHovered=0`），因为鼠标底下是抽屉内部的另一个子窗口；
  `inputText`/`radioButton` 因此画得出来、点不进去。这解释了"输入框点了没反应"，也解释了为什么
  游戏自己的字段没事（它们画在那个子窗口里）。
- **做法**：三个元件的编辑行改在**Mod 自己的一个窗口**里画，尺寸无背景、位置正好盖在面板那几行
  上（`examples/float-ops/components_ui.cpp` 的 `kRowWindowFlags`），每帧 `setNextWindowFocus()`
  让它压在游戏窗口之上——和加载器页面容器同一个调用、同一个原因。行还是原版的样子（左标签、
  右字段），字段是**真的控件**：标签与常量值是 `inputText`（离开字段即提交，和原版面板一致），
  舍入模式是五个 `radioButton`，显示方式是四个 `radioButton`，点了立刻写回配置并请求刷新。
- **弹出的小编辑器整个删掉**：`openEditor`/`drawEditorWindow`/确定/取消/`###TCFloatOps*` 窗口
  都没了，选中元件后不会再有任何窗口盖在棋盘上。`PanelRow.live`（这一行的控件是否被 ImGui
  hover 到）与逐帧 trace 保留，作为"这个构建到底有没有把输入交给插件"的证据。
- **面板几何靠实测而非假设**：行窗口的位置由面板内容原点（`igGetCursorScreenPos` −
  `igGetCursorPosX/Y`）加固定比例算出，换窗口大小/分辨率跟着走；Mod 把每行的行盒与字段盒写进
  `plugin-data/local.float-ops/panel-rows.txt`，自动化用例按这个文件点击，不猜坐标。
- **验收**：`tests/float-panel-playtest.ps1`（真机、真鼠标、真键盘、真存档副本）：驱动用棋盘自己的
  `select_component` 选中 FP32 Constant → 真实鼠标点面板里的标签输入框 → 真实按键输入 `xy` +
  Enter → 日志出现 `float-ops: the label field was committed after editing` 与
  `float-ops: instance 0x… label "xy" (written)`，棋盘上元件名字变成 `name="xy"`，并且**没有任何
  窗口打开**（用例显式断言）。

### 外观这一轮（2026-09-25，晚）：像原版元件的排版 + 引脚挪到本体外

- **元件编辑改在游戏的底部元件面板里**（玩家要求"和原版一样：标签、数值都在面板里改"）：
  - 加载器新增插槽种类 `TC_UI_SLOT_BOARD_COMPONENT_PANEL`（`sdk/tc_mod_api.h`、`sdk/tc_ui.h`
    的 `registerComponentPanel`）。加载器挂钩游戏自己的元件抽屉（`build_toggle_button`，
    见 `src/symbol_profile.hpp`），让游戏先排完自己的标题/说明/引脚图，再把**选中元件的所有者
    Mod** 的回调画进去（带 PushID(mod)/PushID(slot)，并把手上的实例 id 一起传过去）。
    之所以钩标题行而不是抽屉根：抽屉根返回时窗口已经关掉，在它后面画会落到别的窗口；
    之所以不是 punch-tape 用的说明面板：那个构造器只对"有说明面板"的种类运行。
  - 浮点族因此把编辑器从"点元件弹出自己的小窗"改成面板里的两行：`标签` + `常量值`／`舍入模式`／
    `显示方式`（`examples/float-ops/components_ui.cpp`），点击元件不再弹任何窗口。
  - **面板里的行是"摘要"，点一下开小窗**：实测游戏那个窗口不会把插件画进去的控件交给 ImGui 的
    输入路径（行画出来了，点上去毫无反应），所以行只做显示，命中测试由 Mod 自己用引擎的鼠标状态
    做（和它原来打开编辑窗的方式相同），点中哪一行就弹哪一行的编辑窗——这正是游戏自己的模型：
    它的元件数值行也是"点一下弹一个小编辑器"。编辑窗是 Mod 自己的窗口（`tc::ui::panel`），
    输入框、单选框、Enter/确定/取消/Esc 都在那里生效。
  - 配置 schema 2 → 3：三个配置块各追加 `label[16]`（"标签就是显示在元件上的名字"）。
    空标签仍显示元件类型名（原版规则：没标签的 Constant 显示 CONST），有标签就显示标签；
    旧存档按 schema 2 迁移（字段照抄、标签留空）。`tests/float-ops-playtest.ps1` 里驱动给第一个
    常量写了标签 `left`，渲染回调上报 `name="left"` 作为回读证据。
- **编辑值之后棋盘会自己重算**（玩家报"常量设置成 1，实际输出的还是 3.1415925"）：游戏的编译器
  不知道自定义元件的配置变了（它只认识自己的原件），所以暂停中的板子会一直显示上一次算出来的
  结果——玩家看到的"要先刷新一下"就是这个。现在**元件自己**在提交配置之后发出玩家那个刷新请求
  （`tc.simulation` 的 `pause()` = `sim.do` command 1，走同一条钩子链，见
  `examples/float-ops/components.cpp` 的 `refreshBoardAfterEdit`），于是线上标签与下游 Display
  立刻跟着变。放在 Mod 而不是加载器里，是因为一次实测：加载器侧"按仿真是否在出周期"决定刷新时，
  要么每帧读 `sim.cycle`（会让 text-box 用例的相机平移检查失效），要么无条件刷新（会打断
  text-box 的平移）——两者都会影响别的 Mod；Mod 自己发请求则只作用于自己的元件。

- **生成电路的内部布局收进引脚矩形内**（`src/component_definition.hpp`）：玩家报"FP32 Add 右下角多了
  一块"，量出来那块正好落在旧依赖门的电路坐标 `(52,108)`——换算到板面是元件中心右 6.6 格、
  下 13.8 格。原因是游戏会把自定义元件的**内部电路**渲染成一张图（元件栏/工坊/预览都用它，
  游戏自带的 `asset/capture/com_custom_*.png` 就是这种图：内部逻辑 + 引脚点、透明底）。旧的
  依赖门摆在 `(40 + 12i, 100 + 8i)`，于是那张图里多出一块离家很远的方块。现在三列位置固定为
  `collector x=-6`、`dependency gate x=0`、`driver x=6`，门与门、门与整流器的接线重画，
  所有节点都落在**引脚自己的矩形**里（`tests/native-component.cpp` 新增不变量：
  两脚一侧与 16 脚的形状，逐个节点断言 x/y 不越界，并断言节点种类顺序不变）。
  引脚位置与引脚所在行**不变**，所以既有存档的连线不受影响；契约只固定节点顺序，位置本来就自由。

- **引脚通道改成按类型声明**（`TCComponentTypeDefinitionV2::pin_lane`，`sdk/tc_service_api.h`）：
  生成元件的脚由"设计坐标 ÷ 8 格"决定（实测 `-18 -> -2`、`13 -> +2`），声明 `pin_lane = 3.0`
  就落在 `±3`——原版 Constant/Static Value 的 kind 表条目正是 `out0=(3,0)`。默认 `2.0` 保持历史
  坐标（`-18 / +13`）逐字节不变，所以既有 Mod、存档连线与缩略图都不动；越界值直接拒绝注册。
  脚手架的输入/输出走线跟着引脚走（`inputPinX+3`、`outputPinX-3`），这是上一轮"改内部 IO 位置
  就把板子跑死"那个坑的正解：那次只挪了脚，没挪线。
- **绘制表 V2**（`TCComponentRenderDrawV2`，`sdk/tc_component_render.h` 的 `drawV2/textSized/
  measureText`）：按显式像素字号画字 + 量字，宿主只在那一次调用里换掉绘制列表共享数据的字体/字号，
  粗体面取游戏自己的 `NoroshiCode_Bold`。这是"板面文字跟着相机缩放"能对上原版的前提。
- **浮点族按原版排版重画**：本体 4.92 × 2.93 格（原版 Constant 的尺寸）、名字右对齐在右上角、
  位宽框在左上角、值/操作符居中；字号按"数字高 = 0.59 格（值）／0.40 格（名字与位宽）"反推请求
  字号（本构建实测：按请求的 0.79 倍绘制、数字是那一版的 0.66 倍，于是 0.59 格 = 请求
  `0.59 × 每格像素 ÷ 0.52`）。过长的值按本体宽度等比缩小；Display 是十进制一行（0.59 格）
  加位模式一行（0.40 格）。
- **真机外观验收**进 `tests/float-ops-playtest.ps1`：截图后按 Mod 自己上报的逐帧布局（`unit`、
  `body`、`pin0`）量像素，断言三件玩家看得见的事——本体是原版尺寸、名字在右上角且不压值、
  **引脚在本体外面 0.55 格**（原版 0.54）。实测：Constant/Add/Display 本体 4.92 × 2.93 格，
  值 0.59 格（Display 十进制行），引脚外移 0.55 格。

### 浮点元件族 M2（同日早些时候）

- **三个真元件**（`examples/float-ops/components*.{hpp,cpp}`）：`FP32 Constant`（0 入 1 出）、
  `FP32 Add`（A/B → R[32]+Flags[5]）、`FP32 Display`（1 入 0 出）。配置按元件保存，schema 2 的
  每个配置块都以 `format` 字节开头（当前唯一合法值 32），并带一条 schema 1（无 format 的开发期
  布局）迁移路径；写入走 `tc.component.storage`，有 V2 时用 begin/commit 包成一次撤销步。
- **十进制层**：vendored fast_float（正确舍入的解析）与 Ryu（最短往返位），包在 `fp/decimal.*` 后面；
  Constant 编辑器接受十进制、科学计数法、inf/-inf/nan/-0 与 `bits:0xXXXXXXXX`，解析的
  overflow/underflow/inexact 只作编辑器提示，不进元件的 Flags。黄金语料由
  `tools/float-decimal-vectors.py` 独立生成（93 条解析 + 383 条格式化），`tests/float-decimal.cpp`
  另跑 2 万条随机最短往返与缓冲区安全用例。
- **原版风格外观与交互**：沿用原版元件的 footprint 与引脚通道，左上角固定 `32` 位宽框（悬停提示
  FP16/FP64 未实现），其右侧舍入框用紧凑菜单列出五个模式并立即写回；中央操作符、A/B/R/F 标注；
  Constant 与 Display 点本体开小面板（文本编辑 / 显示方式）。C1 显示器的值由逻辑回调写进受锁缓存、
  渲染回调读取，暂停时照常刷新。
- **真机闭环**（`tests/float-ops-playtest.ps1`）：默认 1.0+1.0 显示 `2 0x40000000`；写入
  3.5/1.25 后 `4.75 0x40980000`；半 ULP 用例 1.0+2^-25 在 RNE 下 `1 0x3F800000`，把舍入框改成
  RUP 后 `1.0000001 0x3F800001` —— 配置、内核、显示三段都在游戏里被验证。
- 顺带钉死本构建的引脚换算（设计坐标 ÷ 8 = 板面格，默认通道 2.0 时两脚一侧为
  `in0=(-2,0) in1=(-2,1)`；声明 3.0 通道后为 `±3`），夹具与绘制都按实测对齐。
- 仍未做：点击路径的自动化（菜单/面板的真机点击用例）、Split/Make Bits、FMA/Remainder 元件（M4）、
  TestFloat 资格验证（M5）。

## 未发布 — 浮点元件族 M1：binary32 数值内核（2026-09-25）

- **内核落地**（`examples/float-ops/fp/`）：vendored Berkeley SoftFloat 3e（提交
  `a0c6494cdc11865811dec815d5c0049fba9d82a8`，只取 binary32 的 19 个编译单元，许可证随源码）+
  本项目自己的包装层。公开接口只收发整数位模式：`FPRounding`（RNE/RNA/RTZ/RDN/RUP）、五位
  `Flags`、add/sub/mul/div/sqrt/fma/remainder/roundToIntegralExact、negate/absolute/copySign、
  compare、classify、minimumNumber/maximumNumber 与六个谓词。
- **语义固定**：specialization 用 `ARM-VFPv2-defaultNaN`（canonical `0x7FC00000`、只有 sNaN 置
  NV），每次调用显式设置舍入模式与 tininess-after-rounding 并清空 flags，SoftFloat 的
  `infinite` 映射成公开的 DZ；`platform.h` 把 SoftFloat 状态定义成 TLS，所以仿真线程与 UI 线程
  同时调用互不干扰（真机 DLL 里导出的是 TLS 符号）。
- **独立 oracle 与黄金语料**：`tools/float-vectors.py` 用精确有理数独立实现同一套语义，生成
  `tests/data/float32/fp32-vectors.generated.hpp`——7571 条向量覆盖十二个入口与五种舍入模式，
  值位与五位 flags 逐位比对。oracle 自己先把 960 条最近舍入结果与 CPython double 算术交叉验证，
  再对每条结果做按定义写的性质检查（定向模式的方向与相邻值、最近模式的半个 ulp、开方用平方比较）；
  这两层检查在开发中抓到了 oracle 自身两个错误（RDN/RUP 反了、remainder 符号处理错了），
  所以语料不是内核的自证。
- **fast 门禁**：`tests/float-kernel.cpp` 外加三条契约用例（canonical NaN 与 payload 策略、
  flags 不粘滞、舍入模式不残留）和一个双线程用例（两线程各 20 万次不同舍入模式运算），
  由 `examples/float-ops/build.ps1` 编译执行；`examples/float-ops/build-kernel.ps1` 统一产出
  SoftFloat + 内核对象，Mod DLL、离线测试与真机驱动共用同一份。
- **真机证据**：内核随 `local.float-ops.mod` 打进 DLL，插件加载时自检并把一行结果写进日志
  （`1+2`、`(1+ulp)^2`、fused `fma`、`1/0`、`sqrt(-1)`）；其中 FMA 用例先乘后加会得
  `0x34000000`、fused 得 `0x34000001`，因此这一行同时证明单次舍入的 FMA 真的生效。
  `tests/float-compat-playtest.ps1` 与 `tests/float-boundary-playtest.ps1` 都会断言这一行。
- 仍未做：TestFloat 资格验证（M5 发布前跑，用上游完整源码树单独构建）、十进制 parse/format 的
  vendored fast_float/Ryu（M2）、整数转换（M4，需先定 §12 的 D2）。

## 未发布 — 浮点元件族 M0：独立 Mod 与 32 位宽通路兼容性（2026-09-25）

- **新增 `local.float-ops`**（`examples/float-ops/`）：一个完全独立的原生 Mod，自己带 DLL、构建
  与打包脚本，只使用公开 SDK 与 `tc.component.types`；不修改 `src/native_logic.hpp`、
  `loader.cpp`、游戏 DLL，也不进 `tc-loader.dll`。产物是 `dist/local.float-ops.mod`。
- **M0 交付的是兼容探针而不是算术**：32 位纯源、32 位直通、`R[32] + Flags[5]` 双输出、
  `Flags[5]` 纯汇、32 位纯汇，全部 `state_words=0`。它们覆盖后续 FP32 元件真正需要的形状：
  零输入源、零输出汇、一实例多输出、以及 5 位窄引脚。
- **探针会把真机行为写进日志**：复位段的头几个周期按实例逐条记录它读到的位模式（于是每个
  binding token 都能对上具体值）、每次 UI 刷新记录暂停板上显示的值、每次 RESET 汇总上一段的
  周期/刷新/失配计数。失配计数让“值对不对”由用例判定，而不是靠人眼看日志。
- **真机证据**：21 实例的 `source → pass ×17 → result+flags → sink`（另接 `Flags[5]` 汇）绑定
  token 1–21，四个 32 位形状逐实例读到完整 `0xDEADBEEF`，五位输出被读回 `0x0000001f`；测试
  专用驱动显式走「跑周期 → 暂停 2.5 秒（暂停窗口 `stable at cycle=0`，五类探针各两轮
  `refresh observed` 且值正确）→ RESET → 再跑周期」，复位后的周期线仍旧带完整位模式。
- **token 边界已量化**：42 实例长链上 token 1–33 正确，token 33 是第一个没有状态槽、因而没有
  发布宽输出的 token，它的消费者 token 34 读到 `0x00000000`。与既有“字宽输出最多 32 token”
  的记录一致，失败是确定性的空值，没有越界写入。
- 新增三个用例：`tests/float-package.ps1`（离线形状 + 隔离启停）、
  `tests/float-compat-playtest.ps1`（真机 21 实例 + 暂停/RESET）、
  `tests/float-boundary-playtest.ps1`（真机 42 实例边界，报告写进
  `build/float-boundary-report.txt`），外加 `tests/float-compat.cpp`、`tests/float-fixture.cpp`、
  `tests/float-compat-driver.cpp`。全部记录进 `docs/verification.md`，阶段结论进
  `docs/PLAN-float-components.md` §4.4。

## 未发布 — 拖节点时文本框独占鼠标（2026-09-23）

- **症状**：选中文本框后拖动边框上的缩放节点，棋盘会在同一手势里多出一条导线——真机复现出来的
  那条线两个端点正好是这次拖动的按下点与松开点（`-5,-3 -> -3,-2`，位宽 1）。
- **根因（反汇编 + 真机读数）**：棋盘在 `build_board_ui` 里用 `igIsAnyItemActive` 的回答决定走哪条
  分支，而**光标就在棋盘窗口内**时它走的那条分支照样会调用 `handle_ongoing_action`；也就是说
  "回答有控件在活动" 只换分支，不是棋盘的输入闸门。实测：整个拖动期间棋盘的 **76 次**采样全部被
  答成"有控件在活动"，导线依然出现——所以上一版的拦截不足以解决这条症状。
- **修法**：`handle_io_on_board` 每帧在函数开头读一次四个鼠标按键状态
  （`0x1403558c7`..`0x140355935`）并把它打包进自己的 io 状态；文本框在节点悬停/拖动期间
  **让棋盘读到的按键状态是"没按下"**。四个 ImGui 查询（`igIsMouseDown_ID`、
  `igIsMouseClicked_InputFlags`、`igIsMouseDoubleClicked_ID`、`igIsMouseReleased_ID`）
  只在返回地址落在那一个窗口内时才改写，其它调用者（面板、菜单、ImGui 自己）拿到真实答案。
  原来的采样拦截保留：两者一个换分支、一个换输入状态。
- **真机证据**（`tests/text-box-playtest.ps1`，默认门禁全绿）：

  ```text
  PASS text-box resize handles width=7.000000->10.957347 height=3.600000->5.596587 board-position=unchanged
  PASS text-box resize handles captured board input samples=76 board-objects=unchanged mouse-reads-hidden=608
  ```

  即：尺寸真的变了、元件坐标没动、棋盘的 76 次采样被接住、608 次"棋盘读鼠标"被换成"没按下"，
  而**棋盘自己的元件数与导线数在整个手势前后一字不变**（`components=8 wires=0`）。
- **灵敏度对照**：`TC_TEXTBOX_BLOCK_BOARD_INPUT=0` 用同一段代码、同一串合成鼠标事件关掉独占，
  棋盘立刻恢复原样——`blocked=0 leaked=77`，导线 `wires=0->1` 并在 83 阶段被提交，跑出
  `PASS text-box resize control: without the block the same drag reached the board`。两个方向都有日志。
- 同一轮里隐藏便签的**普通元件拖动照常生效**（`PASS text-box hidden note drag from -6,-18 to -5,-18`），
  所以"节点手势什么都没推动"不是因为棋盘整段失灵。
- 边界：节点独占只在**选中**的文本框节点上成立；鼠标移出窗口后松开导致状态没清的情况由
  "按住的元件已经不在棋盘上就丢弃手势"兜底。

## 未发布 — 文本框直接拖节点调整大小（2026-09-23）

- `examples/text-box` 选中后显示 8 个缩放节点：四个边点只改宽或高，四个角点同时改宽高；拖动时
  即时预览并在松开后持久化。原来的宽高滑条继续保留，作为精确输入和键盘操作的补充。
- 文字块改为贴住当前框的上内边距；放大高度后文字不再留在元件中心，左/中/右对齐都以缩放后的内容区
  为基准。真机门禁新增 `PASS text-box text layout follows resized top edge`。
- 移除一直画在便签中心的固定 8×4 调试轮廓；选中轮廓与 8 个节点现在包住实时可见框。
  `tc.component.geometry` 新增保持 V1 前缀的 V2：`set_instance_footprint` 可在运行期逐实例更新交互矩形；
  宿主把它接入游戏原生 `get_component_id`，返回原生元件序号，因此框内任意位置都能走游戏自己的
  选中、拖动、删除与属性面板流程，不再由文本框 Mod 维护虚拟选中态。
- 类型级 8×4 footprint 仍负责占位和放置碰撞；逐实例 footprint 只扩展原生交互范围，因此放大的
  注释框不会把覆盖的整片电路板声明成不可放置区域。文本框每帧上报最终可见框（包括文字撑开的尺寸）。
- 配置格式升到 v2，新增每实例的 `heightUnits`；旧 v1 数据通过迁移回调补上 3.6 格默认高度，原有文字、
  颜色、宽度和样式不变。文字仍可把盒子撑高，所以节点设置的是最小尺寸，不会裁掉内容。
- 真机用例用真实鼠标事件拖动右下角节点，硬断言宽 `7.00→10.96`、高 `3.60→5.60`，同时元件板面
  坐标保持不变；随后在固定类型 footprint 以外、放大后的框内单击，并断言游戏原生选择集包含该元件。

## 未发布 — 选中提示按 footprint 画（2026-09-23）

- 查清游戏选中提示的机制：`focus_components__...u5084` 只是给**元件自己的网格**换色，自定义元件的
  网格由 `get_mesh_custom__...u2020` 从网格工厂现建，跟 `tc.component.geometry` 声明的矩形无关——
  所以那圈白弧永远按元件网格大小画（实测约 3.9×3.2 板面格，而声明的 footprint 是 8×4）。
- `examples/text-box` 改为自己画选中高亮：通过 `sdk/tc_board_model.h` 读游戏选中集（键 = 元件序列
  索引，`NoteOnBoard` 因此新增 `index`），在 render 回调里对选中实例按声明 footprint 画橙色环 +
  淡填充。真机门禁新增两条硬断言（外接框 = 8×4 格、框心 = 元件中心）；像素回读实测
  `orange hint pixels=1347 bbox=1176,236..1402,350`（8×4 格 @ 28.16 px/格 + 线宽），
  证据图 `build/text-box-out/hint-selected-box.png`。
- **游戏自己那圈弧线也关掉了**：真正的绘制者是
  `redraw_selection__presenterZupdate95state95common_u5104`（`focus_components` 是死靶，棋盘阶段不被
  调用，宿主里那个钩子已删除）。它遍历 presenter 选中容器里的两个哈希集（`+0x00` 元件、`+0x18` 导线），
  每个 0x20 字节的桶按 `+0x08` 占位字是否非 0 决定是否为该元素加一个 selection 精灵实例；桶 `+0x10`
  是元素 id（元件侧 = 元件序列索引）。`tc.component.render` V3 的 `set_selection_hint(id,false)` 因此
  落地为：只在绘制这一帧把该元件那一桶的占位字清 0，调用返回后立刻还原——它不产生精灵实例，别的元件
  与导线完全不受影响，索引空间由游戏自己的 `contains(selected_components, ...)` 判定，不是猜的。
  真机逐像素（同一实例、同一窗口、截图帧里仍选中）：抑制生效 0 白像素，`TC_TEXTBOX_KEEP_ARCS=1`
  灵敏度对照 546。默认门禁与弧线场景都已进 `tests/text-box-playtest.ps1` 的硬断言。
- **原版那圈量清楚了，但没做进示例**：量出原版精灵的几何（圆心 = 元件原点 +1.00 格、内缘半径
  0.80 格、线宽 0.21 格、两段弧 44°–137° 与 223°–315°、随板面缩放），并做了一次原型——照原版断环
  画，半径/线宽/圆心偏移都是参数；同帧对拍（原版白环 + 橙色模仿环）实测模仿环 443 px、原版未被
  盖住的只剩 84 px，外接框差 1–2 px。按 2026-09-23 的决定，便签的选中提示继续用"跟着 footprint 的
  矩形"（它同时就是可拖区，信息量更大），原型连同参数与 4 张证据图
  （`build/text-box-out/hint-imitation{,-both,-custom,-overlay}.png`）撤出示例，登记到
  [reference/unused-interfaces.md](reference/unused-interfaces.md) 第 1 条，要用时按那里的步骤接回来。
- 用例稳定性：修掉"编辑器刚关就发拖动按下会被吞掉"的抖动（先关编辑器，下一帧再按）。

## 未发布 — 按类型去掉"在元件工坊编辑"按钮（2026-09-23）

- 交付 `tc.component.render` **V4**：`set_foundry_button(id, enabled)`。Mod 元件走的是游戏自定义元件那条
  路，选中它时底部面板右上角会出现游戏的"在元件工坊编辑"按钮（`Foundry`），而 Mod 元件没有可编辑的
  内部电路。宿主现在只在那一个按钮的 call site（`build_component_description_panel` RVA `0x3a45ac`，
  `igButton`）上动手：用全透明样式色画出它并返回"未点击"，于是按钮不可见也不可点，面板布局与其它
  控件完全不变；只要当前选中的元件里有一个不是调用方登记的类型，按钮照常出现。
- 真机逐像素（同一个 42×42 px 矩形）：默认关掉 0 像素（中心是面板底色），
  `TC_TEXTBOX_FOUNDRY_BUTTON=1` 的对照保留按钮 654 像素（中心纯白）。ABI 基线 1192 → 1203 条，
  新增记录全属 `TCComponentRenderApiV4`（V1/V2/V3 一条未变）。

## 未发布 — 便签的默认尺寸与随相机缩放（2026-09-23）

- 修用户报的两个问题：**①默认放下来太小**——默认盒子只有 6.5 × 1.5 板面格，而声明的 footprint 是
  8 × 4 格；现在默认配置按 footprint 定尺（字号 0.60 格、最小宽度 7.6 格、盒子最小高度 3.6 格），
  空便签放下就是 7.6 × 3.6 格，"看到的就是能拖的"。**②缩放不同步**——字号、内边距、编辑按钮都带绝对
  像素下限（`max(5,…)`/`max(2,…)`/`max(14,…)`），视野拉大（每格像素变少）时被地板顶住不再缩小，
  文字还会把盒子撑住；现在一切按 `unit` 成比例。证据：`TC_TEXTBOX_LAYOUT_TRACE=1` 在三个相机缩放
  （25.6 / 28.16 / 13.14 px/格）下报告的布局格子数完全一致
  （`box=7.00x3.60 button=0.62 text=0.50 padding=0.22 cells`），这条已进弧线场景的硬断言
  （`PASS text-box note layout stays … cells at 3 camera scales`）。
- 顺带修测试装置：探针中心在相机变动后重新播报（以前只在平移后报一次）；默认绘制的像素窗口收窄到
  ±45×±18 px 并把阈值改成随缩放的比例（`150·(unit/25.6)²`，最低 20），否则缩小时板面装饰的波形线会被
  数成"青色像素"。

## 未发布 — 文本框改回真 0 脚（2026-09-23）

- `examples/text-box` 不再为了"拿到实例"而留一根悬空输出脚：现在注册的是真 0 进 0 出
  （`shape=0in/0out`）。2026-09-22 记下的"没有任何引脚的元件绑不上实例"在本构建已不成立——
  加载器为 0 输出补的那个驱动门是真节点，编译器会把它编进平坦序列，一次运行里 7 个实例全部绑定，
  板上也照常出现在 `tc.board` 的元件序列里。
- 完整真机门禁重跑全绿（放置、rotation 0/1/2/3、缩放、平移、四边裁剪像素、关闭默认绘制像素对照、
  空像素处拖动一格、选中计数 1→0、编辑器读回），另加外观变化：游戏的引脚点不再出现，关闭绘制的
  实例窗口现在是全空。回退开关 `TC_TEXTBOX_OUTPUT_PIN=1` 可切回旧形状。
- 文档同步：`docs/reference/limits.md`、`docs/sdk/services.md`、`docs/research/custom-component-pins.md`
  中"两个方向不能同时为 0"的说法改为"两个都空 = 装饰元件，允许并有真机证据"，
  `docs/research/text-component.md` §1 增加结论更新说明。

## 未发布 — 元件接口 M5：render V2 关闭游戏默认绘制（2026-09-23）

- 新增 `tc.component.render` **V2**（完整包含 V1 前缀，只多一个 `set_default_drawing`）：Mod 在
  `tc_mod_load` 期间为自己拥有的类型关掉游戏给自定义元件画的设计图缩略图、名字水印与相关默认
  网格。宿主接管 `redraw_component__presenterZupdate95state95common_u4925`，只对**属于该类型、
  且带实例 id 的棋盘实例**跳过这次默认绘制；元件栏/元件选择器里的预览（id 为 0 的临时记录）保持
  原样，所以关掉图案不会让元件在菜单里变成空白。
- 关闭默认绘制只影响"游戏画什么"：footprint、命中、引脚数据、选中/删除和本 Mod 自己的 render
  回调都不变。文本框沙箱用同一类型的两个实例做对照——一个只被测量（真机 framebuffer 里默认绘制
  像素 `hidden=0`，同场景开着绘制的对照实例 `visible=350`），另一个被真实鼠标拖走一格
  （`-6,-18 -> -5,-18`，选中计数 1；随后 `clear_selections` 让选中计数回到 0）。三条断言都写进
  `tests/text-box-playtest.ps1`。
- 同一轮把拖动用例的读数污染查清并写进文档：拖动之后游戏会在该实例上画自己的**选中提示 UI**
  （引脚外侧的白色断环），它属于游戏的交互提示层，不是 V2 接管的默认绘制，也不是元件的缩略图；
  因此测量用"从未被拖动"的兄弟实例，严格阈值 `hidden<=4` 才能原样保留。
- ABI 基线按 §20 重跑并评审后更新：新增记录全部是 `TCComponentRenderApiV2` 自身（V1 记录一条未变），
  快照从 1173 条增至 1182 条；`tools/abi.ps1` 现在通过。

## 未发布 — 元件接口 M5：render V1 覆盖层切片（2026-09-23）

- 新增 `tc.component.render` V1 与 `sdk/tc_component_render.h`：Mod 可在加载期为自有类型登记逐实例
  棋盘绘制回调；宿主负责枚举当前棋盘实例并提供帧级句柄、custom/instance id、四向旋转、局部到
  屏幕仿射基、屏幕裁剪框与配置快照。
- 绘制经宿主受控图元表转发（线、矩形、圆、默认字体文本），不外借 ImDrawList/renderer 私有指针；
  所有借用对象只在回调内有效，每实例每帧上限 4096 条，非法坐标/尺寸与超长文本会被拒绝。
- `examples/text-box` 成为首个消费者：为便签声明 8×4 footprint（整框均可拖），并通过正式 render
  回调用宿主仿射基画旋转一致的边框。原有复杂字号/斜体正文暂仍走旧覆盖绘制，等待字体资源切片。
- 新增 render SDK/仿射离线测试，ABI 快照从 1120 条增至 1173 条。当前仍未交付：关闭游戏默认绘制、
  纹理/字体资源生命周期、设备重建通知、缺 Mod 可见占位和完整像素回读验收。
- 变换真机验收补齐第一部分：文本框沙箱一次放置 rotation 0/1/2/3 四个实例，回调把每组旋转轴逆变换
  回统一棋盘基并逐项比较，同时检查四角处于宿主裁剪框；稳定输出
  `PASS text-box render transforms rotations=0xf unit=25.60/25.60 clip=2560x1600`。测试目录现在把该
  PASS 行作为硬断言，旋转轴漏换、符号错误、无效缩放或裁剪框会直接失败。
- render 热路径不再借 `capture_objects` 顺带创建全部 wire 句柄和每帧分配 wire 数组；宿主直接遍历
  当前组件序列，只给实际 custom component 发帧级句柄，复杂布线板的绘制成本不再随导线数额外增长。
- 相机缩放也进入同一真机门禁：自动化在棋盘空白处发送一格滚轮，要求回调中的实时轴长从
  `25.60` 变化为 `28.16`，并输出 `PASS text-box render zoom live unit 25.60 -> 28.16`；若宿主错误地
  缓存旧相机比例，文本框用例会失败。
- 相机平移进入同一真机门禁：自动化在空板上执行中键拖动，记录四个旋转实例平移前后的 render
  原点，要求四者得到同一位移且各自的仿射轴不变；实测输出
  `PASS text-box render pan delta=150.00,0.00 axes unchanged instances=4`。平移后编辑按钮仍按新的屏幕
  坐标命中。
- 裁剪从“只检查矩形数值”升级为真机 framebuffer 像素门禁：诊断模式把裁剪框内缩到无遮挡板面，
  文本框回调用四种不透明色块分别跨越左/右/上/下边；截图逐像素回读得到
  `left=800..831 right=1768..1799 top=200..231 bottom=818..849`，与
  `800,200..1800,850` 的半开裁剪边界完全一致。泄漏一个像素或整块消失都会使脚本失败。

## 未发布 — 元件接口 M5：geometry 第一刀

- 新增 `tc.component.geometry` V1 与 `sdk/tc_component_geometry.h`：Mod 可在加载期为自己已登记的
  类型声明 footprint 矩形，并从实时元件句柄回读游戏实际使用的、已考虑旋转的半宽高；放置路径
  已真机确认。
- 命中判据追查：新增 `tests/component-hitbox-map-playtest.ps1` 与探针的 `TC_HITBOX_SCAN=map`
  模式（63 个网格偏移 + 5 个对照点）。对照点暴露了旧探针装置的缺陷：它用 `SetCursorPos` 驱动
  真实光标、且按下前没有先 hover，测试期间的真实鼠标动作会把采样点顶掉（离元件 40 板面格的空点
  也读成"已选中"，多次运行的地图互相矛盾）。装置已改为按下前一帧先 hover、用 `ClipCursor` 钉住
  光标、逐点记录 `clean=0/1` 并丢弃脏样本。修好后：只按下不移动的手势整窗读 0；按下 + 8 px 拖动
  时旧用例的 `(1,0)`/`(2,0)` 结论可复现（两种 footprint 相同），但只有两点；用拖动手势跑整张
  网格的那次被污染检测判废，干净的全网格仍待补。每次运行的地图追加到
  `build/hitbox-map-out/map-history.txt`；静态调用链地图见 `docs/research/component-hitbox-path.md`。
- 宿主通过游戏自己的 Prototype 复制、Nim 序列分配与 `custom_prototypes_set` 路径写入，不向插件
  暴露原型偏移；旧类型不声明时完全不变。
- 半宽向外量化到整数格，错误线程、越界、非所有者和 stale 句柄均有明确错误码；新增离线几何
  测试与 ABI 快照。真机服务烟测确认 `set_footprint(6,3)` 返回 OK，游戏当前 Prototype 反读为
  `(-6,-3,12,6)`，并在真实棋盘实例上回读 `half=6,3`；研究探针另确认该私有形状序列可稳定
  进入游戏放置路径。
- 本条只标记 M5 已开始：render、资源生命周期、文本框迁移、独立悬停自动化与缺 Mod 可见占位
  仍未交付。

### 修复：命令总线放置的元件现在立刻可点可拖

- `TC_COMMAND_BOARD_PLACE_COMPONENT` 过去只调用游戏的 `add_component`，而游戏自己的放置
  （元件菜单或点板面落件）在那之后还会调用 `upgrade(presenterSlot, 0x30)`。少了这一步，放上去的
  元件**不在棋盘的命中状态里**：鼠标点不到、拖不动，直到棋盘被别的原因刷新一次。
  真机证据：用户手动放一个元件之后，先前用命令总线放的三个元件才一起变得可拖。
- 加载器按自己的惯例**从游戏自身的调用里学习**这个指针：新增符号别名 `board.after_place`
  （`upgrade__presenterZcontext_u2766`），`armBoardRegistration()` 在启动时挂一个只记录第一参数
  再转发的钩子，`add_component` 成功后重放同样的 upgrade。槽位尚未学到时会写明日志，不静默失败。
- 验收（探针不做任何重放）：修复前按内置 AND 门**本体** `selected=0 key=MAX`（只有引脚那格会中），
  修复后按本体 `selected=1 key=1 snapshot=1`，按空板回到 `key=MAX`。命中探针的"选中集永远为 1"
  的闩锁现象也随之消失——那是未登记状态的产物。
- 附带发现（写进 `docs/research/component-hitbox-path.md`）：命中跟随**画出来的图形**，
  `set_footprint(6,3)` 不改变图形（截图里 12×6 与默认 1×1 像素级相同），所以 `set_hit_box` V2
  不能只是声明更大的矩形。

### 结论：可拖区域就是 footprint 框，"整框可拖"不需要新几何

- 真人手测（用户对着探针画在画面上的青框操作，`TC_HITBOX_DRAW=1`）：`set_footprint(6,3)` 的
  **带引脚**类型与**完全无引脚**（0 入 0 出，文本框那种）类型，都可以从框内远离图形、完全空白的
  角落按住拖走。截图见 `build/hitbox-dev-out/boxes-ingame2.png`。
- 因此 M5 **不需要**为"整框可拖"再发明 `set_hit_box`：声明与盒子等大的 footprint 就是那条路，
  而它已经在 V1 里。footprint 同时仍是占位/碰撞框，放大即真的占那么大地方（便签本来就要）。
- 之前"footprint 不改变命中"的说法作废：真正的干扰是①命令总线放置的元件没登记进棋盘命中状态
  （本轮已修，见上）、②"选中集"读数会闩锁、③交替拖动会和邻居碰撞箱互相影响。自动化的移动差分
  网格降级为辅助手段，手感结论优先。
- `sdk/tc_service_api.h` 的 geometry 注释、`docs/research/component-hitbox-path.md` 开头都改成
  这个结论。

### 研究工具：原版元件外观的外形测量（2026-09-23）

- 新增 `tools/component-sprites.js`：从 EXE 的字符串表还原 **kind → 精灵名** 表（125 条，与 125 个
  内置原型逐条对应），并对照 `asset/component_sprites/` 报出画布尺寸与内容框。
- 研究探针新增 `TC_HITBOX_KINDS` / `TC_HITBOX_KIND_LAYOUT` / `TC_HITBOX_KIND_ORIGIN`
  （只放置、不注入，配 `TC_MODLOADER_SHOT` 进程内截图），`build/hitbox-dev/measure-sprites.ps1`
  按 shader 调色板量出可见主体并和精灵画布/内容框对照。
- 结论：**画布以元件记录点为中心，1 板面格 = 20 精灵像素**（四边形 = 画布 / 20；140x100 → 7x5、
  180x100 → 9x5、300x180 → 15x9）；106 个 kind 全量扫描、101 个有缩放读数，中位数 0.0496/0.0495。
  长条/多引脚元件（RAM、Load port、Delay line word、Decoder 3、Level gate、Maker/Concatenator 8、
  OFF）按轴拉伸，是本 kind 自己的矩形。证据与例外清单见
  `docs/research/component-appearance.md` §0，数据在 `build/hitbox-dev-out/sprites-measured.csv`。

## 未发布 — Loader：`tc.io_value` 服务（通用底层）

### 元件接口 M2 回补第三刀：`on_load` 与 `on_save`（2026-09-22）

- **`on_load`**（`TC_LOGIC_LOAD=7`）：实例的配置**来自电路文件里的记录**（原样恢复或经迁移转换）时
  触发，在 `on_create` 之后（复制出来的实例改用 `on_clone`），回调里 `config` 就是文件里的字节。
  **被拒绝的记录不发**——那种情况实例跑默认配置。这样"这是从存档来的"不再需要插件拿字节和默认值去猜。
- **`on_save`**（`TC_LOGIC_SAVE=8`）：游戏**即将写出电路**时对每个活实例触发。两个入口都派发：
  关卡保存（`save.level`，事件总线本来就在监听）与原理图写入（新增别名 `save.schematic` =
  `save_this_schematic__modelZboardZschematics_u132`，别名数 33 → 34）。回调里可以用普通
  `write_config` 提交派生数据或修复配置，**正在写的文件就看到它**。
- **锁纪律**：`on_save` 与 `on_config_changed` 一样在实例锁**外**运行（否则"在 on_save 里写配置"
  会因为同一把非递归锁死锁——第二版实现正是如此，离线用例直接挂住，已修）；`on_create`/`on_clone`/
  `on_load`/`on_destroy` 仍在锁内，别在它们里面调服务。
- **一个实现细节**：编译看到的是扁平化序列，绑定期棋盘记录可能读不到，回读会被推迟到第一次服务
  调用——所以 `on_load` 在"真正把记录装进配置"的那一刻发，而不是死守绑定窗口。
- 证据：离线 `tests/native-component.cpp`（有记录→触发/无记录→不触发；`on_save` 派发到所有活实例，
  且回调里写配置会落到记录里）；真机 `component-storage` 三个模式在 PASS 行报
  `loads=/saves=`，playtest 逐个断言 persist `0/1/1`、migrate `1/1`、reject `0/0`，且每次启动都至少
  有一次保存派发（36s / 26s / 26s 通过）。
- ABI 基线新增 `TC_LOGIC_LOAD`、`TC_LOGIC_SAVE` 与 `on_load`/`on_save` 两个偏移
  （生命周期结构 40 → 56 字节，尾部追加）。
- **M2 回补剩余**：move/rotate/resize 与编译类回调。

### 元件接口 M2 回补第二刀：`on_clone` 与复制语义（2026-09-22）

- **命令总线新增 `TC_COMMAND_BOARD_DUPLICATE_COMPONENT`**：`argument` 是源元件的实例 id，
  `custom_prototype_id` 是它的类型，`x`/`y`/`rotation` 是目的地。放置走游戏自己的助手（于是**一次
  Ctrl+Z 撤掉整次复制**，副本连配置一起消失）；源实例的配置被逐条写进新记录的 tail 表，**不占自己
  的撤销步**——复制对玩家是一个动作。
- **`TCComponentLifecycleV1` 末尾追加 `on_clone`**（结构 32 → 40 字节），`TCLogicPhase` 加
  `TC_LOGIC_CLONE = 6`。它在 `on_create` **之后**、副本配置已就位时触发，回调里 `config` 就是源实例
  那份字节；插件可以在这里发新身份，那次改动是普通 `write_config`，自己占一条撤销。
- **为什么把字节交给绑定，而不是只靠回读记录**：编译看到的是扁平化序列，绑定时棋盘记录可能还读不
  到（实测：源实例的配置回读被推迟到第一次服务调用）。复制路径本来就知道自己抄了哪些字节，所以
  宿主在标记副本时**同时带着字节**，绑定时装进去——`on_clone` 拿到的配置因此是确定的。源实例已绑定
  时优先用宿主手里的配置，记录只作后备。
- 证据：离线 `tests/native-component.cpp`（普通绑定不触发、被标记的副本触发一次且看到抄来的字节、
  `on_create` 也看到那些字节）；真机 `component-undo` 扩展为"两次单笔写入 + 一组 `begin/commit` +
  一次复制"，断言 `clone=d1d2d3d4` 与 `on_clone` 收到同一个首字节，连续两次运行通过（约 15s）。
- ABI 基线新增 `TC_LOGIC_CLONE`、`offsetof(TCComponentLifecycleV1, on_clone)` 与
  `TC_COMMAND_BOARD_DUPLICATE_COMPONENT`。
- **M2 回补剩余**：`on_load`/`on_save`，最后是 move/rotate/resize 与编译类回调。

### 元件接口 M3 收口：编辑事务分组与存档上限实测（2026-09-22）

- **`tc.component.storage` V2**：在 V1 的五个入口后追加 `begin_edit` / `commit_edit` /
  `abort_edit`。批量工具显式说明动作的起点与终点（计划 §9.3 要求，不许按帧猜分组）：begin 记住当前
  字节，中间任意多次 `write_config` 照常改配置、照常发 `on_config_changed`，但**不**各自压栈；
  commit 把整段压成**一条**撤销，abort 还原到 begin 时的字节且不动撤销栈。不嵌套，实例释放/换板
  丢弃未提交事务，与单次写入共用 64 步上限。V1 表继续可查询。
- **真机验收**：`component-undo` 改成"两次单笔写入 + 一次 begin/两步写入/commit"，断言**一次撤销**
  把记录退回 begin 时的字节（不是组内任何中间态），一次重做回到组末态；空转 15.8s 通过。
  离线 `tests/native-component.cpp` 覆盖同一路径（含重复 begin/commit/abort 返回 `ERR_STATE`）与
  "回退也要同步宿主内存里的配置"（`component-undo` 早先只改记录、服务会读到旧值——这是同一类
  陷阱，已修）。
- **1024 字节上限实测**（新用例 `component-capacity`）：探针注册 1024 字节配置，经服务写满后存档、
  重启回读：

```text
run 1: PASS capacity written launch=1 entries=5 file=235 wrote=identical entriesAfterWrite=132
run 2: PASS capacity survived save plus restart: launch=2 entries=132 file=1203 readback=identical
```

  即一个满配实例恰好占 **132 个表项**、字节跨重启一致；原理图从 235 字节涨到 1203 字节，也就是
  **每个满配实例约 1 KB**（原始 132×16 ≈ 2.1 KB，Snappy 压缩后）。顺带记一个现象：游戏在装载关卡
  时会把存档重写成规范化形式（夹具 752 → 235），所以"文件大小"要在装载之后再读才有意义。
- ABI 基线新增 `TCComponentStorageApiV2`（8 个入口）、`TC_COMPONENT_STORAGE_API_VERSION_2` 与
  `TC_COMPONENT_STORAGE_ERR_STATE`。
- **M3 到此收口**：四条验收（存储契约、迁移、缺失 Mod、编辑事务）都有离线 + 真机证据。明确留到
  后续的：缺 Mod 期间画面上的占位元件（并入 M5）、仿真状态是否入档（计划 §18 的决策点）、
  复制（`on_clone`，属于 M2 回补）。

### 元件接口 M2 回补第一刀：`on_config_changed`（2026-09-22）

- `TCComponentLifecycleV1` 末尾追加 `on_config_changed`（结构 24 → 32 字节，全部在尾部；`size`
  没覆盖到它的老调用方不受影响），`TCLogicPhase` 追加 `TC_LOGIC_CONFIG_CHANGED = 5`。
- 触发语义：一次成功的 `write_config`；一次撤销/重做把配置改回旧值。**不**触发：写入相同字节；
  装载存档记录或迁移旧 schema（那是绑定过程，回调在 `on_create` 里就能看到那些字节）。
- **回调在实例锁之外运行**，所以可以在里面调用 `tc.component.storage`；这条不是风格问题——第一版
  实现把通知留在锁内，离线用例立刻死锁（`on_config_changed` 里调 `storage.info` → 同一把非递归
  互斥量），修成"锁内改状态、锁外通知"后才通过。
- 证据：离线 `tests/native-component.cpp` 断言"写入触发一次且看到新字节 / 相同字节不触发 /
  撤销与重做各触发一次并看到被恢复的字节 / 回调内调用服务返回 OK / 记录里的表与内存一致"；
  真机 `component-storage` 三个模式在 PASS 行里带上 `config_changes=`，分别断言
  `1 / 1 / 0`（persist）与 `0 / 0`（migrate、reject）——即"写入算一次改变，装载与迁移不算"。
- ABI 基线新增 `TC_LOGIC_CONFIG_CHANGED` 与 `offsetof(TCComponentLifecycleV1, on_config_changed)`。
- **M2 回补剩余**：`on_clone`、`on_load/on_save`，最后是 move/rotate/resize 与编译类回调。

### 元件接口 M3 第五刀：配置写入进入撤销/重做（2026-09-22）

- **一次成功的 `write_config` 就是一次可撤销的配置提交**。游戏自己的撤销栈没有"配置变更"这类条目
  （它的变更种类都是棋盘编辑，而写值的命令一条都不登记），所以宿主保存这一步的**前后字节**，并在
  游戏撤销入口被按下时，只要还有未消费的配置步骤就把它写回元件记录并报告"已处理"。写入相同字节
  不产生步骤；步骤 LIFO，新写入清空重做栈；每块板最多 64 步；实例释放或换板即丢弃相关步骤
  （记录已不存在，旧 id 可能指向别的元件）。
- **不影响棋盘编辑**：只有存在未消费步骤时宿主才接管这次 undo/redo，栈空时原样调用游戏的撤销/重做。
- **新用例 `component-undo`**（`tests/component-undo-probe.cpp` + `component-undo-playtest.ps1`）：
  经 `tc.component.storage` 写 A、写 B，再用**游戏自己的撤销入口**（命令总线，玩家 Ctrl+Z 走同一条
  函数）撤销与重做，每次读**棋盘记录里的表**而不是服务：

```text
PASS one undo restored the first configuration and one redo re-applied the second:
     instance=2459565876494606882 wrote=a1a2a3a4 then=b1b2b3b4 afterUndo=a1a2a3a4 afterRedo=b1b2b3b4
```

- **测量中排除的两个坑**：① 游戏 Board API 的第一个参数是 `load_level` 接收的那个对象，不是它内部
  +0x78 的表——把 +0x78 当 board 传会让放置与撤销静默失败；② 不能用 `tc.component.storage` 读
  "撤销之后"的配置，绑定会保留内存副本。两条都写进了 verification.md。
- **仍未做**：把配置提交与棋盘编辑合并成同一组事务（`tc.transactions` 目前只暂存命令）、
  缺 Mod 期间画面上的占位元件（并入 M5）、复制（`on_clone`）。

### 元件接口 M3 第四刀：把"缺失 Mod"量清楚（2026-09-22）

这一刀没有改产品行为，改的是**我们知道什么**：在写占位实现之前，先用真机把"缺 Mod 时游戏到底
丢什么"测成了可复现的证据（`component-placeholder` 用例，四个场景，每次启动前清空日志）。

- **缺 Mod 时元件在加载期就被换掉**：槽位变成 kind 0 墓碑（实测 `custom=0 tombstones=1`），
  连线保留、端点仍是原引脚坐标（`wire=0 a=(-6,-1) b=(-12,0)`）。
- **只加载不存档不损坏文件**：`circuit.data` 哈希与夹具一致；装回 Mod 后配置按第三刀的迁移路径
  回来（`migrated ... legacy=1`）。
- **在缺 Mod 的状态下存档会永久丢配置**：文件被改写，再装回 Mod 得到
  `found no instance`——元件与它的记录都没了。
- **"以前装过"不构成保护**：先装 Mod 再移除，元件照样变墓碑；游戏不跨会话保留自定义原型。
- **排除一个想当然的修法**：钩住 `get_custom_prototype__modelZboardZcustom95prototype95list_u451`
  后，整次加载里没有任何一次查询针对夹具里的 id（只看到 `12dc0381356d9010` 两次，且该 id 不在
  夹具中），所以占位必须挂在真正决定丢弃的那段代码上。新增探针
  `tests/component-placeholder-probe.cpp`（只报告，不注册）与真机用例 `component-placeholder`。
- **捕获落点（同日续）**：解析器把整条记录建在自己的栈上——v13 反序列化器的 kind 在 `[rsp+0xe0]`、
  table 在 `[rsp+0x270]`，相差记录内表偏移 `0x190`——所以钩住 `save.custom_tail_set` 就能从
  `table - 0x190` 读出**正在构造的整条记录**：kind、坐标、旋转、实例 id、custom id 和全部 tail
  键值，**缺 Mod 时同样拿得到**。运行时据此新增缺失 Mod 捕获与诊断：启动时接管该 setter、
  `level.load` 链节调原函数前开窗、按实例 id 归档、下一帧判定"该 custom id 没有注册定义且记录不在
  棋盘上"后报告
  `Missing Mod: custom 0x… at (x,y) rotation r was dropped ...; it carries a N-byte schema-S configuration record, kept for a reinstall`
  与一行汇总。用例 `component-placeholder` 新增场景 0 对照并断言"缺 Mod 有该行、元件有主时没有"。
- **救援注入（同日第二段）**：缺 Mod 时把捕获的记录写进宿主自己的
  `<数据目录>/missing-mods/<关卡>.bin`（`TCM3RSQ1` 格式：custom id / 实例 id / 坐标 / 旋转 /
  全部 tail 键值）；下次装载同一关卡时，对"主人已注册且 (custom id, 坐标) 上没有元件"的条目，用
  宿主已有的放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918` 放回棋盘，再逐项
  把 tail 写回，成功后从 store 移除。真机链路：缺 Mod 存档 → `kept 1 record(s)` → 装回 Mod →
  `Missing Mod rescue: put custom 0x… back at (x,y) rotation r with N stored configuration entries`
  → `tc.component.storage` 探针找到实例并完成 schema 迁移 → `kept 0 record(s)`。用例
  `component-placeholder` 原先断言"装回 Mod 也找不回来"，现在改为断言"能从 store 恢复"。
  两点如实记录：实例 id 会变（连线按坐标连接，插件句柄看到新实例）；救援一次性，注入后条目即移除。
- **仍未实现**：缺 Mod 期间**画面上可见**的占位元件（现在游戏仍把元件变成墓碑，我们只在主人回来时
  放回，所以诊断在日志里）；要画面可见需要 M5 的棋盘绘制或占位原型。

### 元件接口 M3 第三刀：老存档的配置迁移（2026-09-22）

- **`TCComponentTypeDefinitionV2` 追加迁移尾字段**（`config_migration_version` / `migrate_config` /
  `migration_user`）。结构体 136 → 160 字节，全部追加在末尾；注册仍只要求首发前缀，老定义不声明
  迁移就得到保守行为。
- **三个明确结果**：`MIGRATE_OK` 把转换后的字节变成实例配置并**升级记录**（下一次存档带走新
  schema）；`MIGRATE_KEEP` / `MIGRATE_REJECT` 都不写回、保留原字节、实例跑默认配置；未知返回码
  一律按拒绝处理，坏回调不会被误当成升级成功。
- **只迁该迁的**：只有"同一个定义 id + schema 或长度不符"的记录会送到回调；来自别的定义
  （`ForeignType`）、校验和不符、格式号未知、超过预算的记录不会被迁移，也不会被改写。
- **回调拿到真字节**：传入的是电路里存着的那份（已通过校验和验证）以及它写入时的 schema 与长度，
  可能和当前 `config_size` 不同；宿主在提交前用自带解码器复核，提交顺序仍是分块在前、校验和最后。
- 记录读取拆成 `component_tail::readStored()`（不管适配，读出 schema/长度/字节）与
  `decode()`（按定义严格匹配），这样"读得出来但用不上"和"记录坏了"不再混为一谈。
- **证据**：离线 `tests/component-tail.cpp`（readStored/decode 两种视角）与
  `tests/native-component.cpp`（转换 / 保持 / 拒绝 / 无回调 / 乱返回码五种路径，含"记录字节一个
  都没变"）；真机新增夹具 `build/nl_not1legacy.data` 与用例 `component-storage-migrate`、
  `component-storage-migrate-reject`：schema 6 的记录被转成 schema 7 并在下一次启动无迁移读回，
  拒绝时两次启动都被喂到同一份老字节。见 [verification.md](verification.md)。
- **仍未做**：缺失 Mod 占位、`on_clone`、Undo/Redo 快照。

### 元件接口 M3 第二刀：配置写进元件存档（2026-09-22）

- **配置随元件存档**：每个实例的配置不再只活在内存里。宿主把整条记录写进元件记录自己拥有的
  64 位键值表（游戏 `save_monger/versions/v7` 的 `Table[int64, int64]`），游戏把它作为元件记录
  的一部分序列化进原理图，所以配置随复制、存档与关卡加载一起走，不需要外置 sidecar。
  键空间高 32 位是 `TCM3` 魔数、低 32 位是字段号；不认识的表项原样保留。
- **绑定即回读**：实例绑定时解码记录，只有格式/定义 id/schema/长度/校验和全部通过才覆盖
  `default_config`，因此 `on_create` 看到的是存档中的配置；拒绝的记录被报告并原字节保留
  （迁移留给下一刀）。
- **写入即记录 + 原子提交**：`write_config` 先构造记录、用宿主自己的解码器复核，再按
  "数据分块 → 定义/格式/schema+长度 → 校验和最后"的顺序提交，最后才改内存；记录写不进去时
  整次调用返回 `UNAVAILABLE`，内存配置保持旧值，不会出现"界面显示新值、存档里还是旧值"。
- **构建画像控制布局**：`compat/profiles.json` 新增 `layout` 段（尾表偏移、元素步长与表头），
  由 `generate-compat.ps1` 校验并生成 `TC_LAYOUT_*`；画像未命中时宿主不装配持久化访问器，
  `info.flags` 的 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE` 为空，配置退回内存层。
- **新的符号别名** `save.custom_tail_set`：游戏自己的表赋值函数
  `X5BX5Deq___modelZsave95mongerZversionsZv7_u70`。宿主只在游戏线程、只在当前棋盘里按
  `(custom_id, instance_id)` 精确匹配的记录上调用它，且从不保存记录或表指针。
- **证据**：离线 `tests/component-tail.cpp` 与 `tests/native-component.cpp` 的持久化段；
  真机 `component-persistence-insert`（空表插入 → 扩容 64→128 → 存档 → 重启回读）与
  `component-storage`（经服务写入 → 存档 → 重启回读到 `aabbccdd`/`01234567`），见
  [verification.md](verification.md)。ABI 基线新增一个 flags 常量，结构体布局不变。
- **仍未做**：schema 迁移回调、缺失 Mod 占位、`on_clone`、Undo/Redo 快照。

### 元件接口 M3 第一刀：配置/仿真状态分离与内存快照（2026-09-22）

- `TCComponentTypeDefinitionV2` 追加配置 schema、字节数与默认 blob（每实例最多 64 KiB，宿主复制）；
  注册改成真正的 size-prefix 兼容，不再要求调用方结构大小必须等于当前头文件。
- `TCLogicIOV2` 追加只读配置视图；RESET 只清仿真状态，不碰配置。
- 新服务 `tc.component.storage` V1 与 `sdk/tc_component_storage.h`：配置原子读写、revision、仿真
  状态捕获/恢复，全部使用 M2 的 generation 句柄拒绝旧实例。
- 离线用例覆盖默认值、短缓冲、schema/长度拒绝、配置更新在下一回调可见、RESET 隔离、状态恢复与
  stale 句柄；服务发现和 ABI 快照同步更新。
- **明确边界**：V1 只是活实例的内存所有权/快照层；跨重启存档、迁移、缺失 Mod 占位、复制和
  Undo 仍待下一刀，见 [HANDOFF-component-m3.md](HANDOFF-component-m3.md)。

### P3：逐周期采集（`tc.sim.capture` + 生成源码注入 tick，2026-09-21）

示波器最要命的那一半：**连续高速跑时一个周期都不漏**。按用户定的"一步到位"，走生成源码注入，
而不是先做"切片运行"。

- **采样点**：加载器在编译源码时，于**每个 `cycle += 1` 之前**插入
  `game_engine.'tc_scope_tick'(U64 cycle)`（实测锚点：生成程序里 `mode_run` 的
  `while cycle < burst_target_cycle` 循环，以及单步路径的另一处；见
  `docs/research/waveform-handoff.md` §4 与 `native-logic-source-*.txt`）。那一刻是"这一拍
  已算完、周期号还没 +1"，所以语义是**周期 N 的稳定状态**；玩家运行、单步、关卡自带测试
  全都经过它。没有 `cycle += 1` 的程序**原样放行**，不被改写。
- **加载器导出 `tc_scope_tick`**：与原生逻辑桥的 `tc_logic_invoke` 同一套注册方式
  （`prepareCompiler()` 把名字→函数指针注册进编译器符号表），未采集时第一句就是一个 relaxed
  原子读 + 返回，热循环里的代价是一个调用 + 一个分支。
- **采集内核 `src/scope_capture.hpp`**：预分配环形（`depth` 拍 = **预触发窗口**）、
  触发（上升/下降/任意边沿/按值匹配 + mask + holdoff）、以及三样必须可靠的记账——
  `gaps`（两次采样周期号不是 +1 的次数，**0 = 没漏**）、`restarts`（周期倒退 = 关卡重置/新一轮，
  整段重记而不是把两次运行接起来）、`injected`（当前程序里到底有没有 tick）。
  仿真线程只写、渲染线程只读，SPSC 单环形。
- **服务与 SDK**：`tc.sim.capture` V1（`configure/start/stop/read/status`）+ `sdk/tc_sim_capture.h`；
  状态里同时给出窗口首尾周期，方便界面画"触发点前后"。
- **离线证据**：新增 `tests/scope-capture.cpp`（`build.ps1` 内执行）——环形窗口与行序、
  行数=拍数、**跳过周期会被计成 gap**、周期倒退会清空并计入 restarts、上升/下降/按值触发、
  停止后不再增长、以及注入本身（插在 `cycle += 1` 之前、缩进保留、没有该语句的程序不动）。
- **真机证据（注入不破坏编译）**：`tests/waveform-playtest.ps1` 照常 PASS
  （"the board panel traced the running level's inputs and outputs per cycle"），日志里
  `Scope capture: per-cycle state reader armed` 与
  `Scope tick: instrumented 2 per-cycle call site(s) so far`（一个关卡编译两次、每次两处
  `cycle += 1`，因此随后涨到 4）——改写后的程序照常编译、照常跑，每一拍都经过 tick。
- **真机证据（服务链路，2026-09-22）**：`tests/scope-capture-playtest.ps1`（新增用例
  `scope-capture`）用 `example.byte-adder` 的自动测试加载 `byte_adder` 关卡并跑满 39 拍，
  驱动 `tests/scope-capture-driver.cpp` 只负责 configure/start/读回：结果是
  `rows=30 written=30 first=0 last=29 gaps=0 restarts=1 injected=1 channels=64`、`consecutive=1`，
  即**一拍一行、一个周期都没漏**；同一时刻渲染线程的 `tc_trace` 采样只有 31 行（每帧一行）。
  驱动读的是状态缓冲的一段（它没有板子句柄，不去猜通道名），用例再用 `example.byte-adder`
  自己回调里逐周期的 `carry_in/a/b` 当日志当**oracle**：状态列 34/33/32 在 7 个可比周期上
  完整复现这三条独立序列——所以采到的确实是关卡自己的数据，不是"看起来很合理"的数。
- **本轮修掉的两个真机缺陷**：
  1. **重复 tick 被当成重启**：生成的程序里有两个注入点，一拍会 tick 两次；旧代码把
     `cycle <= lastCycle_` 一律当重启，于是每一拍清空环形、窗口里永远只有 1 行
     （真机上表现为 `restarts=83 rows=1`）。现在同周期重复上报 = **改写那一行**（后报的值为准），
     不算 gap 也不算 restart；`tests/scope-capture.cpp` 新增该场景。
  2. **通道坐标混用**：`tc::trace::Sampler` 的 `inputSlot()/outputSlot()` 是关卡 I/O
     **历史缓冲**里的槽位，而 tick 读的是**仿真状态缓冲**；混用不会报错，只会安静地采到全 0
     （真机上表现为 `live channels=0/64`）。示波器必须用 `tc.sim.channel` 给的导线状态偏移，
     已写进 `docs/sdk/simulation.md` 的契约。
- **真机待办**：示波器面板本身（P4），建在 `tc.sim.capture` 上。

### 元件接口 M2：实例句柄、生命周期与状态归属（2026-09-22）

按 [HANDOFF-component-m2.md](HANDOFF-component-m2.md) 开工的第一刀，计划书 §7 的四件事
（实例句柄、生命周期、宿主状态 blob、失效规则）都落地了，配置/仿真状态分离与
`on_clone`/编辑器类回调留给 M3。

- **新服务 `tc.component.instances` V1**（SDK：`sdk/tc_component_instances.h`）：
  `enumerate`（按类型过滤）、`validate`、`info`（脚数/状态字数/调用·peek·reset 计数/类型 id）、
  `state`（读宿主状态 blob）、`reset`（只重置一个实例）。句柄带**宿主发放的 generation**：
  槽位被复用后旧句柄返回 `ERR_STALE`，不会解析成"现在恰好在那里"的元件。
- **生命周期**：`TCComponentLifecycleV1{on_create,on_destroy}` 以**追加指针**的形式挂在
  `TCComponentTypeDefinitionV2` 末尾（`size` 未覆盖该字段的调用方行为不变）；相位新增
  `TC_LOGIC_CREATE=3` / `TC_LOGIC_DESTROY=4`。回调在"变化发生的地方"同步执行、写入的状态
  立即提交。
- **失效规则**：编译结果里出现本类型实例的编译才是棋盘本身——其中缺失的实例被释放并触发
  `on_destroy`；离开棋盘/切场景全部释放；释放保留槽位（token 不移动），槽位复用时发放新
  generation。
- **状态归属**：实例状态在加载器的 `Binding` 里按定义大小分配，`0x9a0000` 那 32×512 字节
  只剩"宽输出按位回读"一个内部用途，公共契约里不再出现它。
- **离线证据**：世代校验、枚举/缓冲边界、单实例 reset、释放与槽位复用（`tests/native-component.cpp`）。
- **真机证据**：`pin-shape-wide9` 的实例走查——两个实例 `on_create` 各一次、枚举 2 个且
  generation 不同、reset 只动目标、删除后 `on_destroy` + `released 1 instance(s)` + 枚举变 1、
  旧句柄与伪造 generation 都是 `-4`。原文见 [verification.md](verification.md)。
- **回归**：fast 层 11/11、`byte-adder-smoke`、`custom-or` 的 `multi`（两个 V1 实例）、
  `pin-shape-source`/`sink`/`wide9` 全部 PASS；ABI 基线更新到 1031 条。

### 元件接口 V2：每方向 0–16 脚、自定义状态大小（2026-09-22）

按 [HANDOFF-component-v2.md](HANDOFF-component-v2.md) 一次做完的那一刀，阶段 1 到此收口。

- **新服务 `tc.component.types` V1**（SDK 封装在 `sdk/tc_component_types.h`）：
  `register_definition(TCComponentTypeDefinitionV2*)`。表按 Mod 发放（和命令总线一样），
  因为注册需要调用方的 mod id 与数据目录。返回 `OK / ERR_ARGUMENT / ERR_DUPLICATE /
  ERR_UNSUPPORTED / ERR_GAME / ERR_BUDGET`；被拒绝的定义同样进 `tc.component.registry` 目录。
- **新值模型 `TCLogicIOV2`**：脚数与数组由宿主借用（`inputs`/`outputs`/`state` + `state_words`），
  所以回调能拿到 8 个以上的脚，状态大小也由定义决定，而不是固定的 `inputs[8]/outputs[8]/state[8]`。
  V1 的 `TCLogicIO` 与 `register_component` 一行未动，老 Mod 行为不变。
- **桥接**：`kMaxBridgeInputs/kMaxBridgeOutputs` 提到 16（V1 定义仍被 `supportedScaffold`
  限制在 8 脚），`Binding::state` 改为按定义分配，`invoke`/`reset` 增加 V2 路径。
  `component_definition` 拆出 `validShape()`（形状校验）与 `valid()`（形状 + 回调），
  `encode()` 支持到 16 脚；总输入位宽仍受 128 位 payload 预算约束。
- **离线证据**：`tests/native-component.cpp` 增加 9 脚/16 脚脚手架的节点数、V1 对 9 脚的拒绝、
  128 位通过 / 129 位与 65 位单脚的拒绝。
- **真机证据**：新用例 `pin-shape-wide9`（`tests/pin-shape-playtest.ps1 -Shape wide9`）+
  板子 `nl_wide9board.data`：注册返回 0、实例绑定、编译后的棋盘里 9 个脚/9 个收集门/8 个依赖门/
  1 个驱动门齐全，回调每拍收到 `inputs=9[...]` 并让 `state` 按定义的两个字自增。
- **回归**：`byte-adder-smoke`（V1 声明式）、`custom-or` 的 `shape1`/`shape32`（手写定义）、
  `pin-shape-source`/`pin-shape-sink` 全部照常 PASS；fast 层与 ABI 基线同步更新（985 条）。
- **仍不做**：多字（>64 位）信号（外调只有 4 个参数，且游戏原版上限 64 位）；
  动态引脚、`type_id` 分类/依赖、实例句柄与生命周期属于 M2/M3。

### 元件接口：0 脚元件（纯源 / 纯汇）真机打通（2026-09-22）

按 [PLAN-custom-components.md](../docs/PLAN-custom-components.md) 阶段 1 继续推进。上一份交接把
"N=0（纯源）时谁触发驱动门？M=0（纯汇）时谁来调用回调？"列为必须先有答案的问题；这一轮先测量、
再按结论改接口，测量过程写在 [research/custom-component-pins.md](research/custom-component-pins.md)。

- **测量结论（真机）**：两种形状**都能用"一个悬空的驱动门"解决**。源：0 输入 1 输出，驱动门输入
  悬空，游戏编译后的棋盘里 `node=0x12/parent=<实例>` 仍在（没被剪掉），回调每拍一次；汇：1 输入
  0 输出，尾节点是输出悬空的驱动门，同样被保留、每拍一次。两个形状的导入都返回 0
  （`shape=0in/1out`、`shape=1in/0out`），引脚几何正确。
- **脚手架**（`src/component_definition.hpp`）：`valid()` 允许**一个**方向为空（两边都空仍然拒绝，
  那种元件既不能观察也不能产生）；`encode()` 在 0 输入时不再生成收集门与依赖链、在 0 输出时补一个
  输出悬空的驱动门。节点数公式 `(n ? 3n-1 : 0) + (m ? 2m : 1)`。
- **桥接**（`src/native_logic.hpp`）：`supportedScaffold()` 接受 0 输入或 0 输出；实例节点数期望
  改为 `prefix + outputs + (outputs==0 ? 1 : 0)`，`prefix = inputs ? 2*inputs-1 : 0`
  （旧公式 `2*inputs-1` 在 inputs=0 时于 `size_t` 下溢，会把所有源元件判成"内部电路不认识"）；
  汇的那一个尾节点按"第一个驱动门"发回调。
- **离线证据**：`tests/native-component.cpp` 解出并逐节点断言两种脚手架（源 `[0x12,0x51]`、
  汇 `[0x4f,0x12,0x12]`、三输入汇 9 节点），并覆盖"两边都空要拒绝"。
- **真机证据**：`tests/pin-shape-probe.cpp` + `tests/pin-shape-playtest.ps1`（新用例
  `pin-shape-source` / `pin-shape-sink`），板子 fixture `nl_src0board` / `nl_sink0board`。
  两个用例 PASS：注册 → 绑定 → 每拍一次回调。
- **顺带定下的一条纪律**：0 脚元件几乎不可能满足战役关卡的期望值，而关卡测试一失败游戏就停表
  （实测 `verdict=2`、周期停在第 0/1 拍）。所以真机用例让探针**接管运行**（`simulation.run` 到
  指定周期）再数回调，而不是等关卡测试跑完。
- **>8 脚：游戏侧已测通**（同一探针，`tc_mod_load` 末尾直接构造并导入）：9 / 16 / 32 个输入脚的
  v14 定义全部 `status 0`（`TCComponentStatus::Ok`），ID 依次分配——导入器没有八脚上限。
  所以八脚是**加载器自己的**门槛：`kMaxBridgeInputs` 数组 + 两个 64 位 payload 字的打包
  （**总输入位宽 ≤128** 才是硬约束）。下一步是把数组放大、再用"每脚都接线"的板子做真机运行验证。
- **外调参数宽度的实测与取舍（2026-09-22）**：`TC_MODLOADER_BRIDGE_WIDE=1` 把外调从 4 个参数
  扩成 6 个（追加两个由周期推导、与 payload 不同的字，入口点重算比对）。生成源码确认写对，
  但接收端收到的是垃圾（`0x2eaa7,0x1` / `0x1,0xffffff0000000001`），而 `byte-adder` 仍 PASS
  （额外参数只校验不参与运算）——**四个参数是 JIT 的真实上限**。
  据此定下取舍：每脚 1–64 位（与游戏原版一致）、**不做多字 >64 位信号**、
  **总输入位宽固定 ≤128 位**（脚数可放宽，脚数 × 位宽之和受这个预算约束）。
  代价写清楚：1+64+64 这类元件不在回调接口里做，出口是 M2 的宿主状态/缓冲。
  这条同时解锁 V2 的值模型：每脚一个 `uint64_t` + 宿主借用脚数组，不需要字节视图。
  详见 [research/custom-component-pins.md](research/custom-component-pins.md)。
- **仍未做**：`TCLogicIOV2`（借用脚数组，脚数 >8）+ `TCComponentTypeDefinitionV2` +
  >8 脚的真机运行验证。
- **回归**：`byte-adder`（声明式路径）与 `custom-or` 的 1in/1out 手写定义场景照常通过；
  fast 层 11 项全绿。

### P4：示波器面板 `local.scope`（2026-09-22）

用户要的那个示波器。它不是 waveform-demo 的改皮：数据源换成了 P3 的逐周期采集，
所以快跑时不会跳周期；通道换成 `tc.sim.channel`（板子导线 → 状态字节偏移 + 位宽），
所以采的是 tick 真正读的那块内存。

- **面板**（`examples/scope/plugin.cpp`，`local.scope` 0.1.0，板侧栏）：每条通道一条泳道，
  一位信号画方波、多位网画**数据带**并在跳变处写值；时基 `cycles/px` 可缩放、`<` `>` 平移、
  `Latest` 回到最新；触发（通道 / 上升 / 下降 / 任意沿 / 按值 + 门限）画黄线与 `T` 标记；
  两条游标 A/B（画布上左键点 A、右键点 B），显示 `dt` 与各通道在游标处的值；
  顶部状态行常驻 `rows / gaps / restarts / tick in program`；`Export VCD` 把当前窗口导出。
- **通道来源**：`tc.board` 的对象快照（先 count-only 拿数量再按数量分配——容不下时返回的是
  `ERR_CAPACITY` 且**同时**填好数量，这一点踩过：固定 512 条容量的猜测在这张板子上直接
  拿不到任何通道），再逐条 `tc.sim.channel::fromWire`，最多 12 条泳道，标签写导线两端坐标。
- **真机证据**：`tests/scope-playtest.ps1`（用例 `scope-panel`）——面板注册为板侧栏、
  在真正的板子上解析出 **5 条导线通道**并成功 Arm，随后
  `Scope: first window rows=8 gaps=0 injected=1 lanes=5`，即逐周期采集在真机上有窗口、
  一个周期都没漏。配套 `tests/scope-capture-playtest.ps1`（用例 `scope-capture`）用驱动
  把同一套服务跑到 30 行并逐周期对上关卡自己的 `carry_in/a/b`。
- **每一步都要能看见**：面板把"没有通道"的原因写进日志（没有板句柄 / 板还没导线 /
  快照失败 + 状态码 / 导线没有状态槽），并把 `board@… components=N wires=M` 的变化记一行——
  否则"面板空着"和"服务被拒绝"在日志里长得一模一样。

### 仿真接口收拢成服务：`tc.simulation` V2 + `tc.sim.channel`（2026-09-21）

"先把仿真这层封装好，方便以后开发"——以前这条线散在五个地方（V1 服务只读、命令总线、
五个符号别名、每个 Mod 自己挖偏移、`tc_trace` 的渲染线程采样），现在收成两个服务：

- **`tc.simulation` V2**（V1 前缀原样保留）：`cycle`/`state_size`、`snapshot`（一次读一批
  通道，并回报这次读的周期与 `stable`——读的过程中周期变了就为 0，调用方重读）、
  `run_to`/`run_for`/`step`/`pause`/`reset`（**请求**，走游戏自己的 `sim.do`，因此
  `cycle-guard` 之类的链节照样生效）、`set_slice`（每次运行请求最多推进 N 周期）、
  `control`（请求数、被切短的次数、最近一次的目标与实际生效值）。
- **切片归属加载器**：`sim.do` 的加载器链节现在**总是**安装——它记住仿真模型（V2 的请求
  没有别的办法指名仿真实例）并执行切片；没订阅事件的 Mod 也不会因此多付什么，事件派发在
  没人听时是空的。日志新增一行 `Simulation control sim.do: ok`。
- **`tc.sim.channel` V1**：`from_wire`（板子快照给的导线句柄 → `{字节偏移, 位宽, wire_id,
  两端坐标}`）与 `resolve`（重编译后原地重解析：先按 wire id、再按两端坐标；失活的通道保留
  id、`bits=0`，画成灰的而不是继续瞎读旧偏移）。
- **SDK**：`sdk/tc_simulation.h` 重写为服务层（`tc::simulation::table/cycle/sample/runFor/…`），
  老的 `TCSimulationModel` 保留但改成走**别名表**（`sim.do`/`sim.cycle`/`sim.settings`/
  `sim.setting.get|set`），只有还没有别名的回放缓冲仍按名字取；新增 `sdk/tc_sim_channel.h`。
- **离线证据**：`tests/simulation.cpp` 扩了 V2 与通道服务——表的版本/大小、快照的容量与越界、
  未绑定时返回 `UNAVAILABLE` 而不是野值、`clampTarget` 切片算术（0 切片不切、切片边界、
  重置目标不切、溢出不回绕）。
- **P3（逐周期采集）按"生成源码注入 tick"做**：和原生逻辑桥同一套注入手法（`tc_scope_tick`
  插在周期循环里），这样不论谁在驱动仿真（玩家运行、关卡自带测试）都不漏周期；切片机制保留，
  供不需要注入的场合使用。

### 引脚顺序：整组手柄在"非 2 的幂"条数下消失（2026-09-21）

玩家报的规律是"1、2 有方框，3 消失，4 回来，5–7 消失，8 回来……16 回来"——**只有 2 的幂
才有**。这正好是 Nim 序列容量翻倍的序列，日志也对上了：

```
inputs cache=1 rows=1  drawn=1     ← 1 个引脚
inputs cache=2 rows=2  drawn=2     ← 2 个
inputs cache=0 rows=2  drawn=2     ← 第 3 个：条数读成 0，整组不画
```

- **原因**：面板缓存的记录布局是 descriptor `{count, payload}`，条目从 `payload+8` 开始、每条
  32 字节。加载器当初把 `payload` 开头那个字当成"条数重复一遍"并**要求它与 `count` 相等**
  （那是关卡面板的形状：列表一次性 `newSeq` 好，两者相等）。但面板一条条长出来的列表
  （元件工坊、手动加输入器件的板面）里，那个字是**块的容量**，按 1、2、4、8、16 翻倍，
  所以条数不是 2 的幂时两者不相等 → `viewOf` 判为"不是这个结构" → 整组 `entry()` 全部失败 →
  Mod 一个手柄都没有 ✓ 与玩家观察逐条吻合。
- **修法**：描述符的 `count` 就是条目数（面板按它画），块开头的字只当**上界**校验
  （`容量 ≥ 条数`，且两者都不超过 4096）。`readable()` 的长度也按 `count` 算。
  离线用例新增"3 条记录、容量 4 的列表"：`count()` 读出 3、取第 3 条、整组置换都通过；
  块字小于条数的结构仍然被拒。
- **顺带**：加载器的原始结构转储（`pin order dump: group … count=… header=…`）改成**默认打印**
  且只在结构变化时打印（一次会话最多 24 段）——这种"缓存条数与面板画出来的条目数不一致"的
  问题，靠玩家手动设环境变量才留证据，证据就丢了。
- **证据**：玩家实机日志（上面三行）、fast 用例 `tests/pin-order.ps1` 的新场景、真机
  `pin-order-playtest.ps1 -Drag`（现在的驱动点按钮）`Order: 2,3,4 -> 3,2,4`。

### 引脚顺序：拖动改成 ▲/▼ 按钮（`local.pin-order` 0.4.0，2026-09-21）

"拖动难以实现、bug 太多"——玩家给的方案是换成按钮，就按这个做：

- **每条引脚左边一对小按钮**：▲ 上移一格、▼ 下移一格，左键点一下即生效（`tc.pin_order.move`
  的 `from`/`to` 两个位置，越界自动夹到组的两端）。
- **右键按钮打开"移动格数"窗口**：滑杆 1–32 +「移动 N 格」按钮，同页还有「恢复面板顺序」；
  窗口按引脚 **key** 记录目标，所以放着的这几帧里条目换了位置也不会移错（`indexOfKey` 每帧回查）。
  Ctrl+左键点按钮仍是"恢复该组原顺序"。
- **拖动整段删除**：`dragging/dragGroup/dragFrom/dropIndex/dragKey/targetKey`、
  `dropIndexFor`（按鼠标 Y 找落点）、`keyAtIndex` 与拖动高亮全部删掉——按钮的命中判定是
  自己算的矩形，不依赖"绘制有没有被裁掉"，也就不会再出现"看不见手柄但拖动还在生效"这类错位。
- **绘制**：原来是"左侧抓取条 + 3 个圆点"，现在是两个 15×15 的按钮（圆角底 + 三角），悬停的那个
  变亮；位置以条目**标签行**为基准上下各一个，所以条目高矮都不影响按钮可见。
- **真机证据**：`tests/pin-order-playtest.ps1 -Drag`（这条用例的"驱动"现在改为**点按钮**）：
  日志 `pin order: moved inputs #0 to #1 (status 0)`、驱动 `order inputs before=[2,3,4] after=[3,2,4]`，
  用例断言"前两条互换"通过；截图 `build/pin-order-out/btn1.png` 里每条左边都能看到 ▲/▼。

### 新 Mod：板面网格 + 设置页开关（`local.board-grid` 0.1.0，2026-09-21）

需求："游戏本身已经有网格了，只是看不见，希望画出来，并在游戏设置面板里加一项开关。"

- **网格是什么**：`create_grid__modelZboardZboard_u120`（RVA 0x141390）是游戏的板面格子——它给
  `compute_jps_cache` 的寻路用（`memset 0xc800` 的格子数组），板面坐标本身是整数格：引脚相隔
  1 个单位（`AND` 的脚在 `(-1,±1)`、`(2,0)`），导线在整数坐标拐弯，元件摆在整数格上。游戏只画
  背景花纹，不画这些线。
- **怎么画**：渲染侧不用反推相机——`world_pos_to_screen_pos__..._u1155`
  （别名 `board.world_to_screen`）读全局 `ubo_view_model` 里的相机，返回**归一化**屏幕坐标；
  游戏自己的 `draw_simple_rect`（VA 0x1403404f0，板面框选矩形）就是把这个结果乘
  `ImGuiIO.DisplaySize`（`io+8`）再 `ImDrawList_AddRect`。Mod 用三个探测点（原点、+1 单位 x、
  +1 单位 y）量出仿射变换，求逆得到"屏幕四角覆盖哪一段板面坐标"，然后把每一条整数格线画进
  **主视口的 background draw list**——也就是游戏自己画框选矩形的那一层：盖在板面之上、所有
  面板之下。
- **开关在游戏自己的设置页**：新增别名 `options.general` →
  `build_general_options__presenterZmain95menu95uiZoptionsZgeneral95tab_u339`，Mod 钩它、调用原函数后
  追加一行复选框「显示网格 (board grid)」；状态写在 Mod 自己的数据目录
  （`plugin-data/local.board-grid/board-grid.txt`），不碰游戏设置文件。
- **视觉**：单位格线 alpha 22，每 4 格一条 alpha 46 的线，线宽 1 px；网格只在"游戏认为已加载
  关卡"时出现。
- **证据**：`Symbol profile: 32/32 aliases resolved`、`[local.board-grid] board grid: the Options page
  carries the switch`、`Native loaded: local.board-grid; hooks=1`；隔离沙箱实机截图
  `build/pin-label-out/gridfix1.png` 里网格线与导线、元件位置对齐（导线正好压在格线上）。
- **顺手修的**：`level.loaded` 在符号表里被标成"函数"，其实它是**关卡名的 Nim 字符串数据**；
  文档与 `level.loaded` 的类型说明一并更正——照着旧说明当函数调用会立刻崩在字符串头上。

### 工坊里改引脚后左侧面板不同步：轮子归加载器（2026-09-21）

用户报的两个现象是同一件事："某些数量的引脚不会出现可移动方框" 与 "引脚的标签更改后，
左侧面板不会同时刷新"，两者都要**退出元件工坊再重进**才恢复。

- **先查清缓存是谁在什么时候建的**：`objdump -d` 找 `get_io_states` 的调用者，全 EXE 只有
  一处——`load_level_frontend`（0x316bc6，函数 0x315d50–0x317be0）。也就是说左侧面板那三段
  缓存只在**关卡加载**时重建，工坊里改名/增删引脚都不会走到它，所以"退出再进"是唯一的刷新方式。
  这也解释了"新增的引脚没有方框"：面板画的是旧缓存，新引脚根本不在里面。
- **原修复被自己人挡掉了**：`local.pin-names-patch` 一直负责这件事，它钩 `reload_custom_prototype`
  和 `build_io_state_view` 两个函数；后者在同一版加载器里已经被 `tc.pin_order` 拥有
  （`TC_PIN_ORDER_BUILD_VIEW_RVA` 与 `TC_BUILD_IO_STATE_VIEW_RVA` 都是 0x45ea20），MinHook 对
  同一目标第二次 `MH_CreateHook` 返回 `MH_ERROR_ALREADY_CREATED`，插件整包被拒。用户
  `D:\p` 的 loader.log 里就是这三行：`MH_ERROR_ALREADY_CREATED`、
  `IO state label live refresh unavailable (reload=ok, view=failed)`、
  `Native failed: local.pin-names-patch`。所以引脚编号表也跟着一起消失了。
- **修法：这两件事都归加载器**，因为吃这份缓存的不只是某个 Mod（面板自己、引脚顺序功能
  都吃它），刷新不该取决于玩家启用哪个包。加载器现在自己钩 `reload_this_custom_prototype`
  （0x2e8cd0，同一个已被兼容画像钉住的 RVA），记下"原型重载过"，在 `build_io_state_view`
  入口用游戏自己的 `get_io_states` + 托管 copy/destroy 重建三段缓存，**然后**才套用 Mod 的
  引脚顺序——顺序因此永远作用在刚建好的缓存上，面板与 Mod 看到的是同一份记录。
  两个 detour 合成一个（`hookBuildIoStateView`），standalone 补丁与完整加载器共用它。
- **`local.pin-names-patch` 0.0.4**：不再钩那两个目标（它只留预览侧的 `igText` 与
  `ImDrawList_AddRectFilled`），因此不再被拒；编号表与白条回来了。它的 `report_status`
  现在只说编号表。
- **顺带修掉一个会让整组手柄一起消失的读取缺陷**：`src/pin_order.hpp` 的 `readable()` 原来要求
  `[address, address+bytes)` 落在**同一个** `VirtualQuery` 区域里，而 presenter context 约
  56 KB，很容易横跨两个区域（新分配、堆块刚好跨区、尾部改过保护）。那时对每一组都回答"面板不在
  屏幕上"：没有可排序的条目，画手柄的 Mod 也就一个框都不画——症状同样是"退出再进才好"。
  现在按区域步进（每步用区域自己的 BaseAddress+RegionSize 前进，不前进就拒绝），只有真正
  不可读的页才拒绝。离线用例 `tests/pin-order.cpp` 用两页不同保护（=两个区域）钉住这一点。
- **实机证据**（`tests/live-root-workshop.ps1`，D:\p 实机 + 当前存档 `foundry/2`）：
  加载器日志 `IO panel armed (cache rebuild after a prototype reload + tc.pin_order)`；
  `Native loaded: local.pin-names-patch; hooks=2`（不再 `Native failed`）；
  引脚顺序 Mod 的逐帧摘要 `inputs cache=4 rows=4 frames=4 | outputs cache=2 rows=2 frames=2`
  ——四个输入、两个输出都有框。真机改引脚那一步仍要玩家手动做：改名后 loader.log 里会出现
  `IO panel cache rebuilt (a custom prototype was reloaded)`。
- **`local.pin-order` 0.3.4**：新增两条开发用诊断（`TC_MODLOADER_PIN_ORDER_LOG=1`）：每一帧
  汇总一次"每组缓存条数 / 画了几条 / 画了几个框"（只在变化时打印，行首是 `pin order: summary`），
  以及面板里首次出现的、Mod 还不认识的锚点偏移（`panel anchor +0x… is not handled`，工坊第三组
  就是这一类）。"某个引脚的框不见了"从此可以直接从日志读出是缓存少了、锚点没认出来，还是框被
  裁掉了。逐条目那行（`pin order: inputs #0 key=… screen=(…)`）从"每个进程最多 16 行"改成
  **变化才打印**：它是 `tests/pin-order-driver.cpp` 用来瞄准真机拖动的数据源，而宽引脚自己就会
  打掉好几行方块日志，旧上限恰好会在条目中间用尽——真机 `-Drag` 因此一直没拖成。
- **真机 `-Drag` 用例修好自己的关卡**：`tests/pin-order-playtest.ps1` 默认的
  `symphony_8_io_devices` 只有 1 进 1 出（面板没东西可拖），现在 `-Drag` 自动换成
  `byte_adder`（3 进 2 出），并支持 `-PinLog 1|dump`。这条用例在改动前后都是同一个失败原因
  （旧加载器同样失败），只是现在真的能跑了：日志
  `drag inputs #0 -> #2; before=2,3,4`、`moved inputs #0 (key 2) to #2 (status 0)`、
  `order inputs before=[2,3,4] after=[3,4,2]`。

### 输出标题与纸带重叠 + 最后一框底边被裁（2026-09-21）

实机自定义面板里出现两种剩余布局缺口：`掩码层…` 与“输出状态”叠在同一行，且最后一个输出
（如 `输出 1`）的 bit 可见、边框底边却在子窗口外。

- `local.pin-order` 0.3.3：此前蓝框只知道 bit 所在的第二行，却把“半行距离”当成 bit 自身高度。
  多 bit 时控件很小，误差不显眼；1-bit 控件会放大到约 47 px，蓝框因此从红色 bit 中间穿过。
  2–8 bit 仍在四个光标复位调用点读取 `igGetItemRectSize`；单 bit 不走这些调用点，改在每个条目
  结束的 `igSetCursorPosY` 锚点读取仍为 `LastItem` 的真实控件矩形，并把
  `bit top + 实际高度 + 4 px` 纳入条目下沿。自定义端口原始位宽 `0` 表示自动位宽，和字面值 `1`
  一并走这条路径。宽位数值行及其他 Mod 通过 V2 汇报的矩形继续取并集。`D:\p` 实机高亮回归图
  `build/live-root-pin-order-aa-contained-final2-033-*/out/workshop-live.png` 中，`aa` 的蓝框与
  `输出 1` 的默认框均完整包住红色 bit。
- `local.punch-tape` 0.9.10：真正的 `D:\p` 实机回归发现，333×615 的高工坊先用
  `igSetCursorPosY` 移动绝对行锚点，随后 `igSetCursorPos` 会复用**已经移动过的 Y**来放标签与
  bit；旧逻辑又给后者叠加一次累计高度。两张 64-bit 纸带时 245 px 偏移因此被重复计算，第二输入
  与输出条目被推得过远，而“输出状态”留在掩码行附近。现在高工坊只给标量绝对锚点加偏移，
  完整坐标调用继承其 Y；紧凑关卡面板仍按原路径处理。实机脚本
  `tests/live-root-workshop.ps1` 直接运行 `D:\p\Turing Complete.exe`，按“开始游戏 → 元件树 →
  元件工坊”进入当前存档 `foundry/2`；截图 `build/live-root-foundry-scalar-anchor-fix-*/out/`
  显示“输出状态”独占一行。这个版本只修正内容的位置；单 bit 蓝框的真实控件高度由随后
  `local.pin-order` 0.3.3 修正。
- `local.punch-tape` 0.9.9：面板的标题辅助代码不只在主 EXE，也可能来自重命名后的原始
  `tc_game_engine.dll`。锚点过滤现在接受这两个游戏自有 PE 范围，仍拒绝 Loader/Mod DLL，
  因此标题与输出条目获得相同的累计让位，而 Mod 自己的游标不会被二次移动。
- `local.pin-order` 0.3.2：边框是 draw-list 覆盖层，不会更新 ImGui `CursorMaxPos`；最后一条
  bit 虽然是正常 item，边框下方的 padding 不是，滚动范围因此提前结束。现在用上一完成帧的
  最低本地 Y 提交一个 1×1 `igDummy` 哨兵，再恢复游戏游标；它只扩展内容/滚动范围，不抢鼠标，
  最后一框的底边可以完整滚入视口。

### 最后一条单比特输出的边框底边跑出面板（`local.pin-order` 0.3.1，2026-09-21）

`Carry out` 这类单比特输出本身很大，但截图里真正丢失的是边框底边：它是输出组最后一条，后面
没有新标签来结束当前 `Row`；而旧代码虽然列出了四个 bit-square 调用点，采集时却把面板内**所有**
非标签 `igSetCursorPos` 都写进当前条目的 `squaresY`。后续面板布局因此不断把最后一条的“方块中心”
往下推，最终把底边算到子窗口之外。现在只有已实测的 `+0x21a9/+0x28c8/+0x295d/+0x3257`
四个调用点能更新 `squaresY`；其他绝对定位与该条目无关，不再污染最后一条。V2 内容边界协议保持
不变，它解决 Mod 插入控件的范围；本修复解决游戏原生 bit 方块的调用点归属。

### 引脚条目边界提升到 Loader：`tc.pin_order` V2（2026-09-21）

`local.pin-order` 的外框过去只知道面板的标签/位方块/下一条光标锚点，并不知道别的 Mod 在条目
内部加了哪些控件；最后一条没有下一条可量，靠节奏与调用点猜高度必然会漏。现在 Loader 的
`tc.pin_order` V2 增加按 `{frame, group, key}` 聚合的条目矩形：内容 Mod 用 `include_bounds`
汇报真实屏幕范围，容器用 `bounds` 读取所有贡献的并集，最近三个布局帧由 Loader 保留。

- `local.punch-tape` 0.9.8 在游戏画完随纸带下移的原生数值框、下一锚点尚未生效的时刻，汇报
  整条内部内容的最终范围；
- `local.pin-order` 0.3.0 把该范围并入 `rowFrame`，包括同组最后一条，不再要求存在下一条目；
- V1 排序表仍原样可查，V2 保留完整 V1 前缀；fast 测试覆盖矩形并集、上一帧读取、过期清理、
  V1/V2 兼容以及 `NOT_FOUND`。

### 工坊面板标题错位 + 最后一条把手不贴合内容（2026-09-21）

用户截图：宽输入条目被纸带撑高后，"输出状态"标题压在"掩码层"那一行上；最下面一条的把手框
明显比内容高。

- **标题（`local.punch-tape` 0.9.7）**：让位逻辑原来只认"调用来自 `build_io_state_view` 的代码"。
  关卡面板的标题确实是面板自己放的，但**工坊面板的标题由辅助函数画**——它的返回地址落在面板函数
  之外，于是那一行没被下移，而纸带把内容推下去之后它就压在掩码行上了。现在锚点的接受条件放宽成
  "**面板的子窗口当前正在绘制，且调用来自游戏主程序**"（Mod 自己的调用不算，否则纸带自身的落点会
  被二次移动）。新增开发开关 `TC_MODLOADER_PUNCH_TAPE_LOG=1`（打每个锚点的来源/原值/偏移/终值）。
  真机数据（`symphony_2_io`，`build/pin-order-probe-b/…/loader.log`）：关卡面板里
  `+0x4d2 asked=67 offset=0`、`+0x6d4 asked=67 offset=0`、`+0xd92 asked=96 offset=91 got=187`、
  `+0x1680 asked=149 offset=91 got=240`、`+0x1836 asked=240 offset=91 got=331`——面板内每个锚点
  等量下移、没有重复计数。
- **把手框（`local.pin-order` 0.2.2）**：贴合逻辑里有一条"内容被别的 Mod 撑高时按面板给下一条
  留的位置撑满"的分支。对**最后一条**来说，它后面的不是下一条目，而是面板自己的尾部，于是框被
  撑高（截图里"输出 1"那条框比内容高约一倍）。现在只有"后面还有同组条目"时才用这条分支。
  截图 `build/pin-order-out/fix2-panel.png`（修好后的 `byte_adder`：`Carry out` 的框贴合标签+方块）。

### 元件接口（阶段 1 前半）：类型目录 `tc.component.registry`（2026-09-21）

按 [完整自定义元件接口目标计划书](PLAN-custom-components.md) 的阶段 1 取出的第一块，只做**只读目录**：
引脚/信号可变长那半（0 脚、>8 脚、>64 位）留在下一块，理由与待解问题写在
[HANDOFF-component-registry.md](HANDOFF-component-registry.md)。

- **新增 `TC_SERVICE_COMPONENT_REGISTRY`（`tc.component.registry`）**：两条注册路径
  （`register_logic`、`register_component`）登记的每个类型都进目录，被拒绝的也在，并带
  **第一条**拒绝原因（桥接那句"最多每方向八脚、每脚 1–64 位、总输入 ≤128 位"不会被后续更笼统的
  原因盖掉）。查询入口：`count` / `get` / `find`（按 `custom_id`）/ `pin`（引脚名、位宽、`pin_id`）。
- **声明与桥接的合流**：声明式注册登记的引脚名会保留，桥接时从游戏原型读到的位宽覆盖声明值，
  所以目录里的 `bits` 是游戏真正编译出来的形状。能力位只报告加载器能自行判断的
  `LOGIC` / `WIDE_PIN` / `MULTI_PIN`；计划里其余能力等类型声明阶段。
- **拒绝的 Mod 不留痕**：插件被拒绝时它的类型从目录里删除（与 Hook、界面一致）。
- **SDK**：`sdk/tc_component_registry.h`（header-only 包装）与 `TCComponentTypeInfoV1` /
  `TCComponentPinInfoV1` / `TCComponentRegistryApiV1`；`docs/sdk/services.md` 与
  `docs/reference/limits.md` 增加对应小节。
- **示例**：`examples/mod-inspector` 增加 "component types" 一节（列出类型、形状、代价，
  被拒绝的显示原因）。
- **测试**：fast 层 `tests/component-registry.ps1`（目录语义、拒绝原因、容量、服务表形状）；
  game 层 `tests/component-registry-playtest.ps1`（真机：拒绝项带原因 + `example.byte-adder`
  的 `3in/2out cost=1 delay=1 active=1 caps=0x7` 与五根引脚的声明名与位宽）。ABI 基线随新增
  结构更新。

### 打孔纸带压在"输出状态"上的问题（关卡面板）＋ 锚点提升为钩子链（2026-09-21）

- **`local.punch-tape` 0.9.6**：纸带与"掩码层"按钮从面板左侧那一列之后开始排（`kEntryHandleInset`
  = 20 px，居中范围也相应右移）。面板左边缘是面板自己控件的位置——引脚顺序插件的拖动条就画在
  那里（边框 6 px、抓取条到 13 px、命中到 25 px）——窄面板里宽度接近面板的纸带原本会顶回那一列，
  现在留出来了。截图 `build/pin-order-out/inset1.png`。

- **现象**：在**关卡**面板里，宽输入的纸带（含"掩码层"行）会压住下面的"输出状态"标题与
  输出列表；元件工坊里却正常。
- **原因**：纸带的让位靠移动面板摆放每一行的两个 `igSetCursorPos(Y)` 调用。工坊面板用
  标量那个锚条目，所以推得动；关卡面板的标签用 `igSetCursorPos`（绝对 Y，从
  `igGetCursorPosY()` 读出来再整体设置），只动标量锚点时，标题动了、带着纸带的那一条没动。
- **修法（Loader 层）**：把 `igSetCursorPos` 与 `igSetCursorPosY` 提升为**钩子链**点
  （`TC_HOOK_SET_CURSOR_POS` / `TC_HOOK_SET_CURSOR_POS_Y`），`TCHookCall` 追加一个
  `caller`（游戏那次调用的返回地址），链节可以改 `x`/`y`。这样"谁在这个面板里改布局/画东
  西"不再是谁抢到钩子的问题：
  - `local.punch-tape` 0.9.5：两个锚点都改（按 `caller` 判断是不是这个面板），关卡与工坊
    面板都会把纸带下方整体推下去；
  - `local.pin-order` 0.2.1：改为加入这两条链（优先级 100，晚于纸带的 0），从链里读锚点，
    不再自己钩引擎函数；纸带撑高的条目，它的把手也跟着变高。
- **测试**：`tests/hook-chain.ps1` 新增 `cursor` 场景（`tests/hook-chain.cpp` 用假面板调用
  两个引擎函数：链给出的 caller 必须认得出面板、链节改的 y 必须送到游戏函数、面板以外的调用
  不许被改）。SDK ABI 基线随之更新（`TCHookCall` 56 → 64 字节，纯追加）。
- **真机验收**：`tests/pin-order-playtest.ps1 -Level symphony_8_io_devices -ExtraMods local.punch-tape`
  截图 `build/pin-order-out/chain1.png`：纸带 + 掩码层 + 数值框都在把手里，`输出状态` 标题
  在其下方；`tests/punchcard-playtest.ps1 -PunchTape` 与 `tests/pin-order-playtest.ps1 -Drag`
  同时回归通过（顺序 `2,3,4 -> 3,4,2`）。

### 引脚顺序：`tc.pin_order` 服务 + `local.pin-order`（2026-09-21）

- **需求**：游戏左侧"输入状态/输出状态"面板的顺序是面板自己缓存的次序，玩家改不了；
  玩家希望能拖动面板上的引脚重新排列。
- **底层（Loader）**：新增 `TC_SERVICE_PIN_ORDER`（`tc.pin_order`）与 `src/pin_order.hpp`。
  面板画的不是电路板，而是 presenter context 里三段缓存序列（`io_state_cache.hpp` 的偏移），
  所以"顺序"就是这些元素的前后次序：服务用 `count`/`entry` 描述面板现在显示什么，用
  `move`/`set_order`/`reset` 接受新顺序，并把顺序**在 `build_io_state_view` 入口、面板读缓存
  之前**重排进活缓存——只动缓存、不动电路，值输入框照常可用。顺序存的是 **key（元件下标）**
  而不是位置，因此重命名触发的缓存重建会重新套用，换成另一组引脚时自动失效。
  记录布局（descriptor `{count,payload}`、payload 头部的条数、32 字节记录
  `{元件下标, 位宽, 名字{长度, 数据}}`）由 `TC_MODLOADER_PIN_ORDER_LOG=dump` 在真机上打印，
  对不上时服务选择不动作。
- **SDK**：`sdk/tc_pin_order.h`（header-only 包装）、`TCPinOrderApiV1` 与
  `TCPinOrderEntryV1` 写进 `sdk/tc_service_api.h`；`docs/sdk/services.md` 增加 `tc.pin_order` 一节。
- **Mod（`local.pin-order` 0.2.0）**：给面板每个条目画细边框 + 左侧抓取条（悬停高亮），
  按住抓取条上下拖动即把该引脚放到落点条目的位置，中间的条目依次让位；拖动时目标条目高亮、
  被拖条目描边加粗；Ctrl+点击抓取条恢复该组原本的顺序。几何不是猜比例：面板用
  `igSetCursorPos` 逐条摆放标签与位方块，Mod 钩住这次调用，用"标签锚点"认出条目、用同一
  条目里第二行（位方块）的锚点决定边框下沿，所以边框跟着面板而不是写死高度；绘制走
  draw-list 覆盖层（不提交控件），鼠标只在抓取条那 19 px 内被响应，值输入框的点击与输入不受影响。
- **测试**：fast 层 `tests/pin-order.ps1`（`tests/pin-order.cpp`：用与游戏同形状的假缓存验证
  服务的六个入口，以及"同组重建保留 / 换组失效"两条规则）；game 层
  `tests/pin-order-playtest.ps1`（`-Probe` 打印锚点与服务回答、`-Drag` 用真实鼠标消息拖一次
  并回读顺序、`-Dump` 打印原始记录）。ABI 快照（`tools/abi-snapshot.cpp` /
  `abi/windows-x64.json`）与 `tests/test-catalog.json` 同步更新。
- **验收**：真机 `-Drag`（关卡 `byte_adder`）报告 `Order: 2,3,4 -> 3,4,2`——拖动把第一个引脚
  放到了最后，面板重新绘制后顺序确实是新的；截图 `build/pin-order-out/geo3.png`、`geo5.png`
  （`byte_adder` 与 32 位引脚的 `symphony_8_io_devices`）确认边框贴合"标签 + 位方块 + 数值"。

- **顶部菜单栏插槽 `TC_UI_SLOT_BOARD_MENU`**（2026-09-21）。把"往电路板顶栏挂一个控件"做成
  与侧栏/工具列同一套机制：`tc::ui::registerMenuBarItem(slot_id, draw)` 注册，宿主负责位置、
  样式作用域、ID 隔离、顺序与上限。注入点是 `build_buttons__presenterZboard95uiZmenu95bar_u187`
  清样式作用域时的第一个**真正执行**的弹出（`igPopStyleColor`，RVA `0x45a471`）：探针
  `TC_MODLOADER_LOG_MENU=1` 在两个关卡实测到这一步，而更早的 `0x45a435` 属于可选的"返回关卡"
  按钮标签、普通关卡不执行（第一版就栽在这里：控件注册成功但从不绘制）。绘制在弹出**之前**
  发生，因此 `Button/ButtonHovered/ButtonActive` 三色仍在作用域内，插件用普通 `tc::ui::button()`
  就是顶栏自己的观感；`igSameLine(0,-1)` 把控件接在同一行。示例：`examples/board-panel` 额外注册
  了 `Ping (bar)`；真机截图 `build/menu-bar-out/bar7-top.png` 里它紧跟游戏自己的灯泡按钮，
  日志 `Board menu item example.board-panel/bar drawn in the game's top menu bar (frame 140)`。
  兼容性：新增 RVA 进 `compat/profiles.json`（`-Update` 流程），`TCHost` 与既有服务表未动。
  另外 `Canvas::clicked()` 现在只报告鼠标按下边沿（见上一条同类修复），`tests/ui-draw.cpp`
  增加了"按住不算点击"的断言。

- **被窗口盖住的道具按钮不再响应 hover / 点击**（2026-09-21，玩家反馈）。症状：打孔纸带的掩码窗口压
  在导线调色盘的工具按钮上，鼠标移到被压住的位置，调色盘仍然会被打开。根因在 SDK：
  `sdk/tc_ui_tool.h` 的 `ToolTile` 为了"自己的弹窗盖住自己时不抖动"，把 hover 写成**纯几何**判定
  （只看鼠标在不在 80×80 方块里），完全不问 ImGui 这个位置现在归谁，于是任何画在它上面的窗口都挡不住；
  另外 `sdk/tc_ui_draw.h` 的 `Canvas::clicked()` 直接返回引擎 `igInvisibleButton` 的结果，而这个引擎
  在**按住期间每帧都返回真**（上一轮"点一次纸带闪很多下"就是它），对所有用 SDK 的 Mod 都是同一个坑。
  修复（都在 modloader 层，两个 Mod 不用各自打补丁）：
  - `ToolTile` 的 hover 追加 `ui::isWindowHovered(0)`：鼠标必须在**它自己的窗口**上、且上方没有别的
    窗口遮挡；点击也要求 hover 成立（`clicked_ = hovered_ && canvas_.clicked()`）。游戏自己的对话框、
    别的 Mod 的面板现在都会正常吃掉这个 hover。
  - `Canvas::clicked()` 改成鼠标按下的**边沿**（`pressed && ui::isMouseClicked(0)`），一次物理点击只算
    一次。
  验证：`tests/wire-palette-tool-playtest.ps1`（真机 hover 驱动 + 进程内截图
  `build/wire-palette-tool-out/hoverfix.png`）确认道具在自身窗口可 hover 时照常打开；打孔纸带与掩码层
  用例同时回归。已重新打包并部署 `local.wire-palette` 与 `local.punch-tape`（两者的 `.mod` 都重新构建，
  因为改动在它们共用的 SDK 头里）。

- **新增 `TC_SERVICE_IO_VALUE`（`tc.io_value`）**：把"游戏自己的数值编辑器"那一层收成通用接口，
  供掩码等后续功能使用。表内含 `evaluate`（表达式求值）、`format_value`（按
  二进制/十六进制/无符号/有符号格式化）、`read_input` / `write_input` / `flip_input` /
  `input_width`（按板上元件下标读写全局输入、原生翻转、读声明位宽）、`write_constant`
  （常量完整写入口：设置值 + 运行时值槽 + 刷新，不重编译）与 `write_constant_slot`
  （高级：只要运行时槽）。写值一律按引脚位宽截断；除槽入口外都要求游戏主线程，逻辑线程调用
  返回 `TC_IO_VALUE_ERR_THREAD`。
- **符号画像新增别名**：`io.input.get` / `io.input.set` / `io.input.flip` / `io.constant.set` /
  `io.constant.refresh`（把 `get/set/flip_component_global_input`、`set_setting`、
  `sim_stop_and_refresh` 收进画像，Mod 不再硬编码 Nim 乱名）。
- **SDK**：新增 `sdk/tc_io_value.h`（header-only 包装，含 `truncate`）与
  `tc::ioValueService(host, &api)`；`docs/sdk/services.md` 增加 `tc.io_value` 一节
  （含"为什么 V1 还没调游戏内部 `evaluate`"的说明：它按引用收 Nim 字符串、靠 Nim 错误标志上报，
  Loader 的异常守卫兜不住，要先用探针确认调用形状）。
- **兼容性**：`TCHost` 布局未动（服务表本来就不进 host 结构），`abi/windows-x64.json` 本次为纯
  新增——顺带补上了服务机制引入后一直没刷新的 `sizeof.TCHost=208` 与
  `offsetof.TCHost.query_service=200`，以及此前遗漏的 `TC_HANDLE_ERR_STALE` 记录。
- **测试**：新增 fast 层用例 `tests/io-value-service.ps1`（解析器断言表、位宽截断、服务表形状），
  已挂进 `tests/test-catalog.json`；真机端到端验收随第一个使用方（打孔纸带掩码）一起跑。
- **验收与部署**（2026-09-21）：fast 层 `io-value-service` / `punch-tape-layout` /
  `compatibility-contract` / `sdk-abi` / `save-isolation` / `release-contract` 全通过；真机
  沙盒（`punchcard-playtest -PunchTape`，symphony_2_io）日志
  `Symbol profile: 28/28 aliases resolved`，打孔纸带行为与 0.8.5 一致
  （`grid=313x39`、`entry pushed down by 45 px`）。`dist\tc-loader.dll`
  （SHA-256 `0C2EDDE2…`）已部署到 `D:\p\game_engine.dll`，旧文件备份为
  `dist\tc-loader-before-io-value.dll`（回滚：复制回去覆盖即可）。

## 未发布 — 打孔纸带（`local.punch-tape` 0.7.0 → 0.9.3）

- **0.9.4：去掉掩码的"悬停提示"**（2026-09-21，玩家反馈）。0.9.2 在纸带上方画过一行
  "bit N / 当前值 / 掩码" 的提示（鼠标停在某一格时出现在画布里）。玩家认为没必要，已整块删除：
  现在悬停只保留游戏原版图块自己的高亮帧，`drawGrid` 不再往画布里写任何文字。掩码窗口里
  仍然一直显示 `打孔数据` 与 `电路实际得到` 两行，信息一条不少。

- **0.9.3：掩码改成"一层持续作用的数据变换"**（2026-09-21，玩家澄清）。
  玩家要的是：**掩码位选好之后，纸带上的打孔设置不变，但电路实际拿到的是掩码处理过的值**。
  0.9.2 的"一次性应用"不是这个意思，所以这一版把掩码做成每引脚持久的一层：

  ```text
  纸带 / 数值框  = 打孔数据（你设置的值）
  电路读到的     = 类型(打孔数据, 掩码)        AND v&m / OR v|m / XOR v^m / ANDNOT v&~m / 赋值 v=m
  ```

  实现只用了游戏自己的两个调用入口（都是它原本就在用的）：
  - **读**：面板取引脚值走 `get_component_global_input`。插件早就在这个 UI 调用点上挂过钩子，
    现在当该引脚启用了掩码层时，把**打孔数据**交给面板，而游戏自己的存储保留变换后的值。
    没启用掩码的引脚完全不受影响（钩子只在启用时覆盖）。
  - **写**：任何改变引脚值的路径（原生数值框、插件自己）都走
    `set_component_global_input`。新增的钩子把它解释为"新的打孔数据"：记下来，再把
    `类型(打孔数据, 掩码)` 交给游戏；重入标志保证插件自己的写入不会被二次变换。
  - **常量**沿用宽位常量已有的两个存储：元件的**设置值**放打孔数据（抽屉显示的就是它），
    **运行时值槽**放变换后的值（已编译电路读的就是它），因此不需要重编译。
- 工具窗跟着改成"层"的形式：`打孔数据 …` / `电路实际得到 …` 两行对照、`启用掩码` 开关、
  类型单选、掩码位选择（金圈、全选/清空/反选、表达式）、`固化到打孔数据`（把电路值写回纸带并
  关掉掩码）与 `撤销上一次`；纸带下方的按钮显示 `掩码 ON and 0x…`。
- 验收：真机 `TC_MODLOADER_PUNCH_TAPE_MASK="and:0xFF@0x100"`（沙盒）日志给出
  `mask selftest layer and 0xFF … punch=0x100 panel=0x100 circuit=0x0`——面板保持打孔数据、
  电路拿到掩码后的值，正是玩家要的语义；`tests/punchcard-playtest.ps1 -PunchTape -Mask …`
  断言 `layer=1`。

- **0.9.2：掩码工具改成"先选类型 + 选位 + 预览 + 应用"**（2026-09-21，玩家反馈）。
  玩家的用法是"选择用什么类型的掩码，以及选择掩码位"；0.9.1 把六个动作平铺成一行按钮，
  结果 `AND 保留` 与 `ANDNOT 清 0` 挨在一起被点错——玩家选好 0–7 位（掩码 `0xFF`）、值里只有
  第 8 位为 1（`0x100`）时按到 ANDNOT，得到 `0x100 & ~0xFF = 0x100`（也就是 256），而他期望的
  AND 应当是 `0x100 & 0xFF = 0`。现在：
  - 工具窗顶部先给 **掩码类型**：`AND v&m` / `OR v|m` / `XOR v^m` / `ANDNOT v&~m` / `赋值 v=m`
    （单选，按钮上直接写公式，不再有名字相近的两个 AND）；
  - 中间是 **基准值 → 预览**：`预览 = 类型(基准值, 掩码)`，按"应用"之前就能看到结果；
  - 掩码位照旧用"选掩码位"模式在纸带上点（金圈，不动数值），旁边有 `全选 / 清空 / 反选`
    与 `掩码表达式`（与游戏数值框同语法）；
  - 新增 **"选位时立即应用"**：勾上后每选一位就按当前类型对"基准值"重算并写入
    （始终 `类型(基准值, 掩码)`，不会像逐次施加那样把 XOR 反复翻转）；基准值随面板值走，
    只有工具自己的写入不改变它；
  - 纸带下方的按钮也带类型：`掩码 and 0x100`。
  - 删掉了与 ANDNOT 完全等价的"清除非掩码位"按钮（那是我上一版的重复项，正是混淆来源之一）。
  - 玩家日志确认写入本身可用：连续两个掩码动作的前后值是衔接的（`toggle 0xF8` 之后下一次的
    `before` 就是 `0x0`），所以这次只改交互与命名，不动写入路径。

- **0.9.1：修"点一次位闪很多下"，并把掩码语义写清楚**（2026-09-21，玩家反馈）。
  根因：SDK 的 `Canvas` 把 `igInvisibleButton` 的返回值当成"点击"，而这个引擎在按住期间
  **每帧都返回真**——一次按住于是一次翻转了几十次（玩家日志里
  `workshop input #5 bit 3 flipped` 连着 5 条、`bit 10` 连着 11 条）。现在改用鼠标按下的
  **边沿**（`isMouseDown` 由假变真那一帧）判定：一次物理点击 = 一次动作。按玩家要求也
  **去掉了长按拖动刷位**。
- 掩码命名与说明按玩家的说法重写：开关叫 **"选掩码位（只改掩码，不改数值）"**，选中的位就是
  掩码里为 1 的位（金圈），掩码行写明 `掩码 0x… · 选中 n 位（选中的位=掩码 1）`；动作改成
  **OR 置 1 / AND 保留 / ANDNOT 清 0 / XOR 翻转 / 覆盖为掩码 / 清除非掩码位**（原来叫
  "置1/清0/翻转/保留/清除"，看不出是掩码运算），另有 `全选 / 清空 / 反选`，表达式框标题改为
  `掩码表达式`。选位点击会写日志（`mask bits now 0x… (n selected) on component 0x…`，上限
  12 条），以后看日志就知道玩家选了什么。
- 顺带确认（玩家日志）：掩码运算本身是生效的——`toggle` 0x708 前后值、`AND 保留`、`清除非掩码位`、
  `覆盖为掩码` 的前后值都自洽；问题只出在触发方式与命名。

- **0.9.0：掩码工具**（2026-09-21）。玩家提的用法：原生数值框能写
  `0xFFFFFFFF^(1<<23)` 这类表达式，纸带也应当能"先选一组位、再一次作用到值上"。现在有两条路：
  - **修饰键**：`Shift+点击`=置 1、`Ctrl+点击`=清 0、普通点击=翻转（原行为不变），按住左键在纸带上
    拖动会对划过的格子连续施加同一个动作；
  - **纸带下方的「掩码」按钮 → 弹出工具窗**：`选位模式`里点击纸带只切换掩码（被选中的位画金圈，
    数值不变），工具窗显示 `掩码 0x… (n 位)` 与当前引脚/位宽/值，动作为
    `置1 / 清0 / 翻转 / 保留 v&m / 清除 v&~m`（都按引脚位宽截断），另有 `全选 / 清空 / 反选`、
    表达式框（语法与游戏数值框一致，经 `tc.io_value` 的 `evaluate` 求值）、`撤销上一次`
    （记住写入前的值）与按元件 id 的记忆（掩码与选位状态）。
  - 写入路径：常量走 `tc.io_value` 的 `write_constant`（设置值 + 运行时值槽 + 刷新，不重编译）；
    输入走游戏自己的 `flip_component_global_input`——与原生位方块、以及纸带原有单击完全同一条
    入口，因此周期重置语义不变。**实测结论**（`symphony_2_io`，2026-09-21）：服务里的
    `set_component_global_input` 写的是"周期重置消费的控制回放"，面板绘制的
    `get_component_global_input` 读的是仿真回放，两者要等仿真再次采样才一致；所以掩码对**输入**
    的写入刻意使用翻转路径（原生点击同款），而 `write_input` 留给其它 Mod 按需使用。
    该关卡自带测试会重新施加输入，所以自检日志里"下一帧回读"只作参考（见验证一节）。
  - 排版：掩码行画在纸带与原版数值框之间，行高按游戏实际画的按钮测量；侧栏把这一行加进
    "本项要让出的高度"（0.8.4 的锚点机制），因此不会重叠。
  - 验收：离线 `tests/io-value-service.ps1`（表达式解析/截断/服务表）；真机
    `tests/punchcard-playtest.ps1 -PunchTape -Mask "set:0x100"` 断言
    `punch tape: mask selftest … applied=1 service=1`（掩码算术与写入路径确实执行、服务可用），
    截图 `build/punchcard-out/mask6.png` 里金圈标出被选位、掩码按钮显示 `掩码 0x100`、数值框
    各就各位；回归 `tests/punch-tape-layout.ps1` 依旧通过。
- **调试开关** `TC_MODLOADER_PUNCH_TAPE_MASK="<op>:<表达式>"`：对面板里第一个宽位元件执行一次
  掩码并写日志（`applied=` 与下一帧 `readback/match=`），供沙盒验收使用。

- **0.8.5：纸带避开面板滚动条**（2026-09-21）。0.8.4 让下面的内容下移后，面板内容超出高度时
  会出现竖向滚动条；ImGui 把滚动条画在窗口**内部**，而纸带是按整个窗口宽度测量并居中的，于是
  右侧最后一列孔位被滚动条盖住（玩家截图：`aaaaaa123` 最右一列只露一半）。
- 现在纸带按“可见内容宽度”测量与居中：用 `igGetScrollMaxY()` 判断该子窗口是否真的在滚动，再用
  `igGetContentRegionAvail()` 加当前光标 X 得到内容区右边界（滚动条在它外侧）。窗口不滚动时
  仍按整个窗口宽度，所以正常情况下孔位不会因为这条修复变小。
- 同时把侧栏的字节组间距从抽屉的 28/44 收到孔位的 2/5（0.8.4 已给 `makeLayout` 加了
  `preferred` 参数，这里只改侧栏自己的那份，底部抽屉不变），同样宽度下孔位再大 1~2%：
  实测 32 位 `symphony_11_functions` 由 cell 1717 变 1739。
- 调试开关 `TC_MODLOADER_PUNCH_TAPE_SCROLLBAR=<px>`（与加载器的 `TC_MODLOADER_TRACE_*` 同类）：
  假装右侧有这么多像素的滚动条，用来在没有滚动的面板上检查这条路径。
- 验证：真机 `TC_MODLOADER_PUNCH_TAPE_SCROLLBAR=18` + `tests/punchcard-playtest.ps1 -PunchTape`
  日志 `geometry workshop_input7 … grid=295x36 cell=1639 group=655`（按 18 px 让位并居中，右侧
  留出滚动条的位置），截图 `build/punchcard-out/scrollbar-panel.png`；不滚动的面板
  （`symphony_8_io_devices`、`symphony_11_functions`）仍是 `grid=313x39`，尺寸不受影响。
  离线用例 `tests/punch-tape-layout.cpp` 增加“让出滚动条后仍能整组换行、右边缘不越界”。

- **0.8.4：侧栏纸带保持孔位不变，改为把下面的内容顶下去**（2026-09-21）。玩家截图里 64 位输入
  `aaaaaa123` 的纸带把原版数值框挤到了下方“输出状态”标题上。根因：这个面板**不按内容堆叠**
  ——`build_io_state_view` 每画完一项就把下一项锚回自己那套固定节奏（`90 × 游戏 UI 缩放`，
  333 px 侧栏在 606 px 高窗口里实测 121 px），所以一项自己的高度既挤不开下一项，也推不动
  后面的内容。
- 先按“把纸带缩到本项空出来的高度”做过一版（那样 64 位在这块面板里只剩 36 px，孔位要掉到
  约 7 px），**玩家否决：孔太小**。现在的做法反过来：纸带尺寸继续用 0.8.2/0.8.3 的侧栏 band
  （矮面板 ≤48 px、高面板 80–110 px），把纸带的高度加到面板内每次 `igSetCursorPosY` 上——
  下一项、循环结束后的“输出状态”标题、以及整段输出列表一起下移（真机日志
  `punch tape: workshop entry pushed down by 44 px`）。
- 加多少是实测的：原版一项 = 标签 + 数值框，节奏留给它的间隙是 `节奏 − 标签 − 数值框`
  （333 px 侧栏实测 121 − 18 − 58 = 45 px）；加“纸带高 + 画布下方 6 px”正好把这个间隙原样
  保留，所以带纸带的一项留白与原版一致，且不与任何东西重叠。
- 挂钩范围：`igSetCursorPosY` 只处理返回地址落在 `build_io_state_view` 内部的调用（本构建该
  函数 0x3fc0 字节，符号表里下一个符号即其结束），因为输出段用的是它自己的锚点、不是输入循环
  那一个；`igBeginChild_Str`（返回地址 `ioStateView+0x394`）在每个面板开始时把偏移清零，帧号
  变化也会清零。两者都原样转发。钩子装不上时只影响留白（纸带仍按 0.8.3 画）。
- 顺带：1/4 孔位的行距现在由 `makeLayout` 的 `preferred` 参数带进布局，不再事后压缩，同样的
  band 能换到更大的孔（矮面板里的 32 位输入 cell 15.7 → 17.2）。
- 验证：`tests/punch-tape-layout.cpp` 侧栏小节改为断言“孔位不缩水 + 顶下去后仍保持原版间隙”；
  真机 `tests/punchcard-playtest.ps1 -PunchTape`（`symphony_2_io`，32 位输入）日志
  `geometry workshop_input7 … cell=1717 grid=313x38` 与 `workshop entry pushed down by 44 px`，
  截图里纸带、数值框、“Outputs”标题与输出项各就各位（`build/punchcard-out/grow2.png`）。

- **0.8.3：收紧工坊纸带行距**（2026-09-21）。保留 0.8.2 放大后的孔位尺寸，将侧栏纸带的
  纵向行距限制为孔位尺寸的 1/4；64 位四行整体缩短约 15px，为游戏固定位置的“输出状态”标题
  留出空隙，避免最后一行与标题重叠。

- **0.8.2：放大工坊侧栏孔位**（2026-09-21）。每行最多两组的规则不变；高度超过 430px 的
  工坊左侧面板把纸带高度预算从最多 48px 提高到 80–110px，64 位仍为 4 行 × 2 组，但孔位可从
  约 7px 放大到约 17px。较矮的关卡 IO 面板继续使用紧凑预算，避免覆盖输出区。

- **0.8.1：工坊侧栏改为最多两列**（2026-09-21）。左侧输入纸带现在每行最多显示两个 8 位组
  （16 位），宽位输入优先向下增加行数；底部常量抽屉仍保持每行最多四组。

- **0.8.0：元件工坊左侧输入设置支持宽位纸带**（2026-09-21）。原版
  `build_io_state_view` 只在位宽 ≤8 时绘制纸带，更宽的输入只剩数值框。Mod 现在在该界面读取
  输入值的专用调用点补画同一套游戏图块，1–8 位原版路径保持不变；点击调用游戏原生
  `flip_component_global_input`，继续沿用它的周期重置语义。侧栏使用比底部抽屉更小的独立缩放下限，
  仍严格每 8 位一组、最多每行 4 组，并按当前侧栏宽度选择整组换行与缩放，保证整条纸带不出界。
  真机 `symphony_2_io` 的 32 位输入在 333 px 宽面板中显示为 2 行 × 2 组（286×39 px），点击位
  5/4 均进入原生翻转函数；用例为 `tests/punchcard-playtest.ps1 -PunchTape -NoPatch`。

- **加载器的源码 dump 改成按需开启**（2026-09-21）。`src/native_logic.hpp` 过去每次编译都往
  **游戏目录**写 `native-logic-source-N.txt`（关卡一次编译两个程序，所以一次进关至少两个文件；
  正式安装目录里已经堆了 59 个、约 4.0 MB）。现在只有设 `TC_MODLOADER_DUMP_SOURCE=1` 才写，
  默认一个字节都不落盘；验收脚本自己带上这个变量，并新增 `punch-tape-playtest.ps1 -NoDumpEvidence`
  专门验证"不设变量时目录是干净的、关卡照常编译运行"。已归档的 59 个历史 dump 移到了
  `tc-modloader\build\legacy-source-dumps\`。

- **0.7.3：纸带随窗口自适应**（2026-09-21）。此前纸带的尺寸与纵向位置是按一次测量写死的
  （44px 单元、抽屉内 y=119），窗口一放大它就显得又小又贴顶，抽屉一矮则可能整排画到面板外。
  现在几何算术独立成 `examples/punch-tape/tape_layout.hpp`：优先按整字节组换行、行数尽量填满
  （不留 24/24/16 这样的半截行），再把整条纸带缩放进「抽屉剩余宽 × 高」这块空间，最多放大 1.5 倍、
  最少缩到 0.25 倍，尺寸由 `tc::ui::windowWidth()/windowHeight()` 现场读取。用户那块 2555×1450
  窗口上，18 位常量从 1217×44（44px 单元）变成 1826×66（66px 单元，正好铺满可用宽度）；同一块
  电路在 1462×914 的老窗口里，64 位常量由「一列 520px 高、会溢出抽屉」变成 4 行 ×16 位、553×171
  完整可见。用例：`tests/punch-tape-layout.ps1`（fast 层）；预览：
  `tools/preview-punch-tape.cpp <out.bmp> <面板宽> <面板高> <位宽,...>`。

- **症状**：在 73.2k 门的电路上点纸带的一格，界面要停约 1 秒才更新。
- **定位**：`set_setting` 只改元件记录，模拟器用的却是编译期写死的常量字面量；原路径随后调
  `upgrade(presenterContext + 0x1a3b8, 0x30)`，游戏按字面量重新生成源码并重编译整张电路。
  延迟因此来自整板重编译，UI 侧怎么调刷新顺序都没用。
- **修复**：加载器的代码生成钩子把 9–64 位常量从字面量改写成
  `U64 game_engine.'tc_dynamic_constant'(U64 <component-id>, U64 <fallback>)`（refresh 与 cycle
  两阶段都改），在编译器 foreign 表里注册该桥，并导出 `tc_dynamic_constant_set`。纸带点击改成
  写设置 → 更新运行时槽 → 刷新现有仿真，不再请求重编译；遇到没有该导出的旧加载器时自动回退到
  原来的重编译路径。
- **真机验收**（2026-09-20，玩家自己的 `sandbox`／RV32I 电路）：两阶段各 6 行改写、关卡跑通
  （`autotest finished cycle=39 verdict=0`）、三次更新 0.001–0.008 ms 且源码 dump 数不变、
  从 `#SIMULATION_STATE + 332` 回读到的值与纸带写入值一致；真人鼠标点击纸带的两条记录同样自洽
  （bit 3 → 11、bit 12 → 4107）。脚本与证据见
  [HANDOFF-punch-tape-runtime-constants.md](HANDOFF-punch-tape-runtime-constants.md)。
- **已知未尽**：原版数字输入框仍按原版语义重编译；运行时值槽的高频读取仍带互斥锁；验收脚本的
  合成鼠标点击还没落地（真人点击已经通了）。

## 未发布 — WordWatchee 64 位数值标签（新包 `local.word-watchee-64`）

- **症状**：字长 >32 位的棋盘上，导线与端口的数值标签只显示低 32 位（高 32 位整段消失）。
- **定位**（证据见 [research/word-watchee-64.md](research/word-watchee-64.md)）：模拟器把整字
  读成 uint64（`sim_state_read_u64`）并按 8 字节上传，着色器也已经写好 33–64 位的十六进制、
  无符号与有符号十进制；丢失发生在绘制侧：`word_watchee_mesh.set_value_size` 写 instance 属性
  前执行 `min(value_size, 32)`（函数 +0x40 处 `mov eax,0x20` / `cmp bl,al` / `cmova ebx,eax`）。
  同一路径另有独立一处：`word_watchee.vert` 的 `repr16()` 64 位分支把低 32 位的起始位移写成
  `32`（应为 `28`），会多写一个半字节并把低位数字整体错位一格；32 位字上的 `>> 32` 还是
  GLSL 未定义行为。
- **修复（新包 `local.word-watchee-64`，format 2）**：原生插件把该立即数改成 `0x40`
  （即 `min(value_size, 64)`，上限取 64 是因为着色器用 `1u << (size-33)`，>64 会未定义），
  写入前逐字节核对本构建的函数窗口，不匹配只写状态不动代码；着色器那行由包的精确文本补丁
  改掉，停用包时从备份还原。1/8/32 位棋盘的代码路径逐字节不变。
- **包 1.1.0：位宽 >32 位改成双排十进制**。64 位十进制一行 20 位太长，所以宽标签画成两行，
  换行落在**十进制数字**的第 10 位之后：上排是前 10 位数字、下排是剩下的数字（不足 10 位时
  下排显示 0），例如 2^63 = 9223372036854775808 显示为 `9223372036` / `854775808`；宽标签
  **不跟随**游戏的十六进制／无符号／有符号格式。位宽 ≤32 位的标签完全不变，仍按游戏设置的
  格式单行显示。实现要同时改顶点与片元着色器（片元按行寻址字形），所以包里改成**整文件部署**
  `word_watchee.vert` + `word_watchee.frag`（停用即从 `blobs/` 还原原文件），顶点着色器里
  `shift = 28` 那处修正也在这份文件中。真机截图见
  [verification.md](verification.md#wordwatchee-64-位数值标签2026-09-20)。
- **包 1.1.0 的真机回归修正**（同一版内，用户截图发现）：① 第二行曾被截断成一位——真机调试
  确认数字提取正确，坏在**动态下标局部数组**（`uint scratch[20]`）在这块驱动上的读写只保留
  一格，现已改成"先数位数、每个数字直接写进目标格子"，着色器里不再有任何中间数组；
  ② 游戏自带的 `divmod10`（`ceil(2^67/10)` 逆乘）对 **2^63** 差一（余数 7 vs 真值 8），
  vanilla 只用 ≤10 位所以没暴露，本包换成四段 16 位长除法。真机截图：2^64-1 →
  `1844674407`/`3709551615`，2^63 → `9223372036`/`854775808`（与用户给的例子一致）。
  离线用例新增两条断言（不得出现 `scratch[`；必须用 `/ 10u` 长除法）。
- **验证**：`node tests/word-watchee-model.js` 从**钉住的 EXE**读函数窗口、从**包里的
  `.vert`** 解析 `ROW_STRIDE` 与两行语义，断言 >32 位的两行十进制（含边界值）与 ≤32 位单行
  仍跟随格式；`tests/word-watchee-playtest.ps1` 断言两个着色器与包内文件逐字节相同、`state.json`
  记录的前后哈希与备份、插件在真机读回的位宽字节、进入棋盘截图；`tests/word-watchee-layout-playtest.ps1`
  用探针把位宽强制成 64，在真实 Byte Adder 棋盘上截出双排标签（diagnostic 层）。
  尚未取得"64 位棋盘上高 32 位真的画出来"的画面证据，原因写在研究记录 §6。

## 未发布 0.6.0 — 符号画像与钩子链（地基第二项）

- **修复 `level.load` 的关卡名载荷**：加载器现在把钩子参数里的 Nim 字符串转换成回调期间有效的
  C 字符串传给 `TC_EVENT_LEVEL_LOAD`，不再固定派发空值；事件名指针仍是借用值，Mod 不得跨回调
  保存。离线宿主测试用真实 Nim 字符串布局覆盖转换，真机 `game-handle-probe` 要求两次加载都得到
  非 `(none)` 的名字。
- **Board V4 对象数据读取**：新增 `read_component` / `read_wire`，把 V3 签发的句柄变成可读对象。
  元件暴露 kind、原理图坐标、方向字节、板内 64 位 id 与自定义原型 id；导线暴露它自己的序列下标
  （游戏 `get_wire` 的 id）、端点、位宽和仿真状态字节偏移。字段全部来自指定构建已核实的记录位置，
  语义未核实的实例父链与引脚描述符**没有**进结构。读取要求句柄与记录仍然属于当前棋盘序列，枚举
  之后被编辑或跨帧都会以 STALE 拒绝；离线契约用例覆盖解码与边界，真机探针逐条读回每个对象并断言
  id 唯一、位宽与状态槽在实测范围内。真机还测出两条只有实机能给的事实：元件序列第 0 条是全零
  占位记录（kind 0，不是元件），以及 `level.load` 帧内的棋盘可能还没稳定（3 元件/1 导线 vs
  相邻帧 1/0），两者都写进了 `docs/sdk/services.md`。
- **引脚几何与位宽封装**：`sdk/tc_game_model.h` 新增 `tc::pinPoint()`、`prototypeInput/OutputPinPoint()`、
  `prototypeInput/OutputPinWordSize()` 与 `pinWordSizeIsAuto()`，把“描述符起点是 `TCPin* + 8`”
  这条锚点算术收进 SDK——此前每个调用方都要自己加。`tests/kind-list-probe.cpp` 改用这些入口后，
  真机重新 dump 的 126 行与改动前逐行一致（125 个内置元件的名字／引脚数／偏移／字宽全同），
  这是封装正确性的真机证据。
- **Board V5 元件引脚服务**：新增 `read_component_pins`，给一个元件句柄就能拿到它的引脚列表
  （相对坐标、声明位宽、原始 WordSize 与 `AUTO_SIZE` 标记），宿主用元件自己的 kind 或
  `custom_prototype_id` 取原型、在同一调用内复制并释放，没有任何借出的原型指针离开调用。
  内置 kind 只使用启动时从游戏 `PROTOTYPES` 表枚举出的集合，未知键绝不交给游戏。真机探针在战役
  关卡读到的 `0x3c`（0 进 1 出，`(1,0)`，w1）与 `0x44`（1 进 0 出，`(-1,0)`，w1）与
  `build/kinds.txt` 里同一构建的条目逐字段相同；`kind == 0` 的占位记录按游戏自己的空原型读成
  0 个引脚（日志 `zero=1`），不伪装成元件。
- **`tc.simulation` 仿真读数服务**：新增 `get_state`（周期、引擎帧、状态缓冲大小）与
  `read_value`（读某个字节偏移的低 N 位，语义与游戏自己的 `sim_state_read_bits` 相同）。导线记录
  里的状态槽偏移与位宽因此可以被直接取值。偏移按模拟器初始化时分配的 10,240,000 字节缓冲做界内
  检查，越界返回 `RANGE` 而不是野读；沿用"状态在步进结束时才写好"的时机规则。服务层由真机探针
  验证（别名武装、界内、掩码与游戏一致），"值随周期变化"的正确性由波形用例负责。
- **P1 编辑命令族起步：命令 V2 与元件放置**：新增 `TCCommandApiV2` / `TCCommandV2`（保留 V1 前缀，
  追加放置载荷）与 `TC_TRANSACTION_API_VERSION_2`，命令总线因此能表达"在某个格点放一个元件"，
  事务也能承载它。关键证据是放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918`
  的第一个参数就是 Board model——句柄正好解析到它，所以编辑不需要 presenter 上下文，也不用
  自己写 UI 自动化。真机 `component-placement-playtest`：`submit=0` → `state=3 result=0`，复用一个
  kind 0 墓碑槽（序列 2→2、活元件 0→1），并按 V3 枚举 + V4 读数在 `(20,6)` 找到 kind `0x4e`、
  自定义 id 全对的新元件。同一次运行还把两个编辑放进 V2 事务（`begin → stage×2 → commit`）：
  `state=4`（COMMITTED）、`staged=2 completed=2`，棋盘序列 2→4 并在两个目标点都命中——命令、
  事务与公开回读三段串起来验过。
- **删除元件入口完成第一轮核实**：反汇编确认
  `board_delete_component(component_sequence,index)` 的第一个参数是 `board + 0x78`；真机按“放置 →
  删除 → V3/V4 回读”测到序列 `2→2`、活元件 `1→0`、原坐标查无记录。删除与 undo 一样留下 kind 0
  墓碑，下一次放置会复用该槽。这个低层入口没有自己写撤销历史，因此尚未直接进命令总线；已找到
  会调用 `add_undo_changes` 的 `try_delete(board, component_indices, wire_ids)`，下一步是先真机验证它的
  delete/undo/redo，再定命令载荷。移动候选 `board_commit_move` 只提交已完成的拖拽并依赖选择集，
  没有目标坐标参数，也暂不接入。
- **修正导线记录的端点读法**：`TCWireInfoV1` 的 `x`／`y` 曾被当成"一个端点的坐标"，实际
  `+0x18` 起是**两对** int16（x1/y1 与 x2/y2）。P1 的加线实验推翻了旧读法，两个独立证据给出新
  解释：and_gate 内置解的导线是 `(-6,-1)->(-13,-1)` 的水平线，战役关卡的是 `(9,0)->(-9,0)`，
  正好落在输入/输出引脚之间。结构已改为 `x1/y1/x2/y2`，真机断言同步更新。
- **导线编辑的结论（暂不进总线）**：`add_wire_from_pos` 只需 board+点+颜色，但真机实验显示它
  只"起一条线"——记录零长度、状态槽为奇数、游戏自己的 `get_wire(point)` 拒绝寻址。把它做成队
  列命令会在棋盘上留下半成品，因此 `PLACE_WIRE` 不进契约；要做真正的程序化连线需要新的反推或
  一次钩子归属决策，证据与两条路线写在 [research/board-object-fields.md](research/board-object-fields.md) §4。
- **Board V6 连接判定**：新增 `read_wire_ends`，用一个调用回答"这条线的每一端连在哪个元件的哪个
  引脚上"。规则是玩家看到的几何规则（引脚位置 = 元件位置 + 引脚偏移），实现只比较邻近窗口内的
  元件，候选元件用与 V5 相同的内置/自定义原型来源。真机：战役关卡那条线两端分别解析到 0x44 与
  0x3c 两个元件句柄，方向与引脚序号正好是它们的端口，V4 回读确认 kind。
- **Undo/Redo 语义实测（编辑族第 4 项）**：真机用一个**两步事务**（连续放两个元件）测出两件事：
  ① 一次 `BOARD_UNDO` 只回退**一步**，所以"事务 = 一步撤销"不成立，想让玩家一次 Ctrl+Z 抹掉整批
  得自己按步数 undo；② 被撤销的元件**不会缩短序列**，而是留在原地变成一条 kind 0 的全零墓碑，
  于是"数元件必须跳过 kind 0"（`component_count` 只是序列长度），Redo 会把该记录还原。两条都写进
  [sdk/commands.md](sdk/commands.md) 与 [reference/limits.md](reference/limits.md)。
- **`OBJECTS_CHANGED` 上真机（编辑族第 3 项的一半）**：探针在自身 stop+save 事务结束后，通过命令
  总线放一个**内置**元件（0x04），下一帧收到**恰好一次**结构变化事件；活元件数 0→1（稳定帧读数，
  跳过 kind 0 占位）。`SELECTION_CHANGED` 仍只有离线覆盖——写选择集还没有已核实入口，两者一起
  列在待办。
- **选择集写入的线索**：用 `nm` 在 `modelZboardZboard` 命名空间筛出候选
  （`select_component`、`incl`×2、`excl`、`clear_selections`、`get_selection`，以及和撤销栈直接相关的
  `add_undo_changes`），全部标注为**签名未核实**，列在
  [research/board-object-fields.md](research/board-object-fields.md) §5，等下一轮反汇编 + 真机探针。
- **实例父链的结论**：元件记录 `+0x10`／`+0x18` 经两次真机 dump 确认在**板级记录上是空的**，
  读它们的代价求和与代码生成路径都跑在编译期扁平序列上。因此 Board V4 继续不暴露它，理由是
  “它不属于板块”，而不是“还没测”；要暴露应另开编译图服务。见
  [research/board-object-fields.md](research/board-object-fields.md)。
- **游戏 API 不再继续堆进 `TCHost`**：新增 `query_service` 与 `services` 能力，服务 id、版本、
  输出大小分别校验；第一张 `TCBoardApiV1` 表承接当前 Board 句柄的查询、验证和受控解析。
  既有三个句柄入口原样保留给旧二进制 Mod，新接口以后按 Board/Simulation/Command 等服务独立
  演进。离线测试覆盖未知服务、错误版本、短缓冲区、旧宿主回退和真实句柄生命周期。
- **Board V2 同帧只读快照**：新增不含借用指针的 `TCBoardSnapshotV1`，一次调用复制 Board
  身份、引擎帧、仿真周期和当前/上一份选择计数。捕获只允许在游戏主线程执行，并在读取前后
  复核 Board 句柄与帧号；数据源缺失由 flags 明示，重入、失效句柄和短缓冲区都有独立错误码。
- **Board V3 对象句柄**：`capture_objects` 用调用方缓冲区原子枚举完整 Component/Wire 集合，
  容量不足只报告所需数量、不发布半份结果。子句柄只能由这条可信枚举路径签发，并在下一引擎帧
  或 Board 生命周期结束时自动失效，避免把会搬迁的游戏序列元素当成长寿命裸指针。
- **命令总线 V1**：新增 `tc.commands` 服务，把 run/stop/reset 意图排到本帧插件回调之后统一
  执行；每条请求绑定提交 Mod 和 Board 句柄，执行前二次验证生命周期，并提供 QUEUED 到终态的
  查询与取消。队列、每 Mod 未完成数和历史结果都有硬上限，插件拒载时自动取消其请求。
- **值类型生命周期/变化事件 V1**：新增 `tc.lifecycle` 服务，统一报告 Board entered/left、对象
  结构变化与选择集合变化。事件携带句柄、帧/周期、单调序号和带 flags 的计数；结构指纹只覆盖
  已核实稳定字段，选择指纹覆盖实际 ID，避免轮询、同数量换选漏报和仿真缓存噪声。
- **事务、Undo/Redo 与 Save**：命令总线接入游戏原生 Board undo/redo 和关卡保存入口；新增
  `tc.transactions`，在 begin 时记录 Board 结构指纹，commit 时统一预检冲突并连续执行最多 32
  步，可选仅在全部成功后保存。V1 明确不伪造通用自动回滚，失败会报告 completed_count，补偿走
  游戏自身 undo 历史。
- **首个稳定游戏对象句柄**：新增 `TCGameHandle` 与 `game_handles` 能力；Board 在关卡加载时
  签发，下一次加载或场景切换自动提升代次并作废旧句柄。插件可查询、验证并受控解析句柄，
  不再需要长期保存无生命周期信息的 `void* board_model`。组件/导线/关卡 kind 只预留、不接受
  任意裸指针，等待统一快照从可信枚举结果签发。宿主 ABI 仅在尾部追加三个入口，旧字段偏移不变。
- **Board 句柄的真机验收，和它逼出来的一处缺陷修复**：新增只读探针
  `dev.game-handle-probe` 与自驱动版本 `dev.game-handle-probe-driver`，以及游戏层用例
  `game-handle-probe`（`tests/game-handle-probe-playtest.ps1`）：探针从主页入口进关卡、
  按主页关卡入口进入、按 Escape（玩家自己的离开动作）退出，两次进出都是玩家的路径，全程只用
  公开 ABI。
  真机实测暴露：游戏先 `level.load`、再在**同一帧**切到棋盘场景，旧实现把这次切换也当成
  “离开”，于是刚签发的句柄当场作废，整个关卡内 `get_current_game_handle` 一律返回
  `UNAVAILABLE`——只有真机生命周期能发现这一点。现在按引擎帧号区分（加载那一帧或下一帧的
  切换算进入），并把 `scene.change` 提前到**加载任何插件之前**由加载器自持：插件对该目标
  `create_hook` 会被明确拒绝并提示改用 `TC_EVENT_SCENE_CHANGE`，而不是抢先拿走 detour、
  静默关掉句柄失效机制。`TC_EVENT_SCENE_CHANGE` 的 `subject` 现在带上**游戏自己传给
  `change_scene` 的 context**：加载器拿走这个目标后，驱动（和 Mod）否则就再也拿不到它——
  board-panel 用例的“切场景后棋盘停止绘制”正是因此退化成 NOTE，补上 `subject` 后恢复为硬断言。
  探针随后还按**玩家的动作**验了离开路径：关卡内按 Escape，游戏自己调用 `change_scene(ctx,0)`，
  句柄随即失效（`DRIVER: self-exit=ok`），驱动自造的切换只保留为回退分支。
- **游戏兼容性与 SDK ABI 成为发布门禁**：新增 `compat/profiles.json`，统一声明支持构建的
  EXE/原引擎哈希与大小、全部 Loader RVA、所需符号别名和可升级旧 Loader；生成头供安装器与
  运行时使用，`tools/compat.ps1` 可在安装前离线识别。新增 Windows x64 ABI 基线与编译探针，
  对 193 项公开结构大小/对齐/字段偏移及稳定常量逐项比较；画像 id、清单哈希和 ABI 基线哈希
  写入 `release.json`，fast/host 测试与正式发布都会阻止未审核漂移。
- **发布基建收口**：新增声明式 `release/manifest.json` 与 `tools/release.ps1`，发布包从空
  staging 按清单收集，缺失、重复或未通过 Mod 契约校验即失败；公开示例不再靠
  `package.ps1` 手工 `Copy-Item`，补齐 board-panel、byte-adder、waveform 与 inspector。
  `.mod`、源码包和玩家包统一使用固定 ordinal 条目顺序、固定 ZIP 时间戳与压缩参数；
  `build.ps1` 固定 GNU ld 写入的 PE/COFF 时间戳，安装器、原生 DLL 与 CLI 在两次完整重编译后
  仍字节一致。同一 staging 连续打包两次必须 SHA-256 相同。最终目录携带玩家包内外两层
  `SHA256SUMS.txt` 与机器可读 `release.json`；fast 层新增 `release-contract` 回归。
- **插件不再硬编码混淆名**：新增 `src/symbol_profile.hpp`——一张"稳定别名 → 本构建 COFF 名"
  的表（20 项：`sim.do`、`sim.cycle`、`sim.settings`、`sim.setting.get/set`、`sim.state.read`、
  `level.load`、`level.loaded`、`scene.change`、`board.wire.update`、`save.count`、
  `save.path.*`、`runtime.emutls`、`ui.fonts`、`ui.pushFont`、`ui.invisibleButton`、
  `cost.gate/total/delay`）。加载器启动时整表解析一次并写日志；真机日志实测
  `Symbol profile: 20/20 aliases resolved`——表里每一项都在游戏里核实过。
  插件侧 `tc::resolveAlias(host,"sim.do")` / `resolveAliasAs<SimDo>(...)`，取不到就是 null，
  不会静默拿到错地址。
- **多个 Mod 可以共用同一个游戏函数**：加载器为已核实签名的钩子点安装**唯一**的 detour，
  插件各自加入链节（`tc::hook::addSimDo(host,priority,&callback,user)`）。链序为
  优先级升序 → Mod id → 注册顺序，日志给出成员表；现开放 `TC_HOOK_SIM_DO` 与
  `TC_HOOK_LEVEL_LOAD` 两点。语义：返回 0 继续、非 0 停止链；`call->skip_original=1`
  让游戏自己的函数不执行（替换行为）；需要看返回值时先 `call->run_chain(call)`，
  原函数保证**至多运行一次**。被拒插件不留链节。
- **链上目标被保留**：对 `sim.do` / `level.load` 调用原始的 `create_hook` 会被拒绝并提示
  改用 `register_hook_chain`——这正是"两个 Mod 都想钩 sim_do"从必然失败变成彼此共存的地方。
  其余目标（如界面驱动用的 `igInvisibleButton`）仍可用原始 Hook，重复目标照旧冲突。
- **迁移**：`example.cycle-guard`（sim.do）、`example.waveform-demo`（level.load）与
  `dev.sim-state` 探针都改走链与别名；示例清单相应声明 `symbol_alias` / `hook_chain`。
  真机证据：waveform 沙箱 `Hook chain level.load installed with 1 link(s)`；
  sim-state 沙箱同时启用 `example.cycle-guard` 时
  `Hook chain sim.do installed with 2 link(s): dev.sim-state@0, example.cycle-guard@0`，
  关卡照常跑完、映射断言全过。
- **验证**：新增离线用例 `tests/hook-chain.cpp`（假的 `sim_do`/`sim_get_cycle`/`load_level`
  符号 + 探针包）与 `tests/hook-chain.ps1`，断言三链节的顺序与参数传递
  （1000→1001→1002→1003，原函数只跑一次）、吞掉原函数的链节、以及"普通目标可原始钩、
  链上目标被拒"；`tests/native.ps1` 的冲突场景改为两个包抢同一个普通目标
  （第一个 ok=1、第二个 `Hook rejected`）。细节见
  [reference/symbols.md](reference/symbols.md)。

## 未发布 0.6.0 — 事件总线（地基第三项）

- **"关卡加载了 / 玩家按了运行 / 游戏保存了"不再需要每个 Mod 自己钩内部函数**：新增
  `sdk/tc_event_api.h` + `sdk/tc_event.h`，`host->add_event_listener(context, kinds,
  callback, user)` 订阅 `TC_EVENT_LEVEL_LOAD`（subject = 板模型、name = 关卡名）、
  `TC_EVENT_SCENE_CHANGE`、`TC_EVENT_SIM_COMMAND`（flags = 0 run / 1 stop / 2 reset）、
  `TC_EVENT_SAVE`（flags = 保存计数）。`kinds` 是位掩码，`TCEvent.kind` 是到达的那一位，
  一个监听器可以同时处理多种事件。
- **事件的来源都由加载器拥有**：`level.load` 与 `sim.do` 走已有的钩子链，加载器把自己的链节
  放在最高优先级（`-2147483647`），因此即使别的 Mod 吞掉了这次调用，事件照样发出；
  `scene.change` 与 `save.level`（新加入符号画像）在需要时才挂钩，没有监听器就完全不碰。
  启动日志写明每个事件源的状态（`Event source level.load: ok`、`Event source save armed`…）。
- **被拒插件从总线上摘除**：和 Hook、链节、界面、元件同一套生命周期。
- **验证**：`tests/hook-chain.cpp` 的事件场景（假 `sim_do`/`load_level`/`change_scene`/
  `save_level_data` 符号 + 一个订阅全部四种事件的探针包）断言四种事件都到达、顺序与调用一致、
  负载正确（`command=0`、`count=3`、`subject`），并且不影响游戏自身函数的行为；
  `tests/hook-chain.ps1` 额外断言四个事件源都被装载。真机：`example.waveform-demo` 改为订阅
  关卡加载事件取板模型（原先自己进链），波形／VCD／导线探针断言全过，日志
  `Event source level.load: ok` + `Waveform: board model capture subscribed to the level-load event`。
- **尚未做到：逐周期回调**。查证结论写在
  [verification.md](verification.md#仍未做到逐周期回调)：仿真的周期循环跑在游戏自己生成的
  机器码里（`compile_asm` / `compile_isa`），EXE 符号表里没有"跑一个周期"的函数可以钩，
  所以波形在"连续运行"下仍按帧采样、会跨周期。要真正做到每周期一次，需要像原生逻辑桥那样
  在生成的源码里插入回调，属于后续独立工作。

## 未发布 0.6.0 — 崩溃署名与日志轮转（地基第四项的一半）

- **插件崩溃会被署名，但不会被隔离**。先按计划做了崩溃遏制（VEH + `setjmp/longjmp`，
  因为 MinGW 的 GCC 没有 `__try/__except`）：该机制在孤立环境里四种情形全部恢复
  （`tests/fault-guard-probe.cpp`，`build.ps1` 内执行），但接到加载器的链节上时，处理函数执行完进程仍以
  `0xC0000428`（`STATUS_INVALID_IMAGE_HASH`）退出；把故障点改到加载器自己的受保护代码块
  结果相同。因为"一半时候管用的安全网比没有更危险"，最终交付可依赖的那一半：
  `tc-modloader-data/fault.log` 记录 `dev.hook-chain-crash in sim.do: access violation at 0x…`
  ——Mod id、当时在跑的回调/钩子点、异常类型与地址，然后让进程照常崩溃。
  完整实测与"要做到真隔离需要什么"写在 [verification.md](verification.md#安全模式与崩溃隔离)。
- **日志不再无限增长**：`loader.log` 超过 8 MiB 时轮转为 `loader.log.1`（只保留上一份），
  行格式不变，因此既有断言与文档中的日志样例都仍然作数。此前该文件在实测目录里已有 6.9 MB。
- `tests/hook-chain.ps1` 新增 `crash` 场景：探针包故意写空指针，脚本要求进程非 0 退出**且**
  `fault.log` 同时写明 Mod id、钩子点与原因。

## 未发布 0.6.0 — 能力协商与依赖版本约束（地基第一项）

- **插件不再靠"宿主结构有多大"猜能力**：`TCHost` 尾部新增 `host_version`
  （用 `TC_HOST_VERSION_CODE(0,6,0)` 打包）、`capabilities`（`TC_CAP_*` 位掩码）与
  `report_status(context, level, message)`。位只有在对应入口**确实实现**时才置上，
  承诺与实现同一处生成：`src/capabilities.hpp` 的 `loader_capabilities()`。
  SDK 侧给了 `tc::hostHas` / `tc::hostCapabilities` / `tc::hostVersion` /
  `tc::reportStatus`，它们自己检查 `host->size`，因此在没有这些字段的旧加载器上安全
  返回 false/0，插件不必再手写 `offsetof` 比较。
- **包可以声明自己要什么**：`mod.json` 新增 `capabilities` 列表（`log`、`symbol`、
  `hook`、`logic`、`component`、`ui_page`、`ui_slot`、`texture`、`status`）。
  扫描阶段就区分两种拒绝并给出可读理由——`Unknown loader capability: x`（作者写错名字）
  与 `This loader does not provide the capability: x`（加载器比包旧）。后者是本次改动的
  重点：以前这种情况要等到 `tc_mod_load` 里静默失败。
- **依赖可以带版本约束**：`requires` 除 id 数组外接受 `{"id": ">=1.2.0"}` 对象形式，
  新增 `optional`（可选依赖没启用就不检查，启用则照样检查）。约束支持
  `= != >= > <= <`、逗号联合与 `*`；版本比较取开头的点分数字段，所以
  `1.10.0 > 1.9.0`（纯字符串比较会弄反），尾随文字被忽略。校验在写入任何文件之前完成，
  失败时磁盘保持原样，Mods 页显示
  `X requires Y >=1.2.0, but the enabled version is 1.0.0`。
- **Mods 页跟着变**：详情栏显示依赖及其约束、可选依赖、包声明需要的加载器能力；
  插件通过 `report_status` 报的状态按级别（信息/警告/错误）着色显示在同一行。
  加载器启动日志新增一行 `TC Mod Loader 0.6.0 capabilities: …`，加载每个原生包时记录
  它声明的能力。
- **验证**：新增离线用例 `tests/capabilities.cpp`（能力表完整性、名字规范、
  `VERSION` 与 `TC_MODLOADER_VERSION_STRING` 一致、版本序、约束语义、两种依赖写法与
  四种拒绝路径、文档与代码的能力名单一致），`build.ps1` 内执行；`tests/run.js` 增加
  11 个包管理场景（`--version` 与 `VERSION` 一致、`--capabilities` 名单、能力声明通过、
  未知能力被拒、约束满足/不满足/数字序/可选缺失/可选启用后仍检查/约束类型错误），
  共 35 项全通过。
- 细节与表格见 [reference/capabilities.md](reference/capabilities.md)。

## 未发布 — 元件预览：引脚编号 + 表格

- **从 loader 内置功能拆成可启停 Mod**：完整 Mod Loader 不再编译、安装或自动启用引脚显示钩子；
  新包 `local.pin-names-patch.mod`（v0.0.3）通过宿主 `create_hook` 注册预览文字、引脚白条、
  自定义原型重载和 IO 状态面板四个钩子，只有玩家启用该 Mod 时才生效。宿主的原始钩子校验相应
  扩展为接受两类安全目标：符号画像中的 EXE 函数，以及 `engine_proc` 返回且确实位于原引擎模块
  可执行页内的导出；重复目标仍会被拒绝。免 Mod Loader 的独立 `game_engine.dll` 补丁继续保留。
- **修复原版 IO 状态面板不同步**：在工坊下方修改输入或输出端子标签时，画布元件会立即使用
  新名称，但左侧“输入状态/输出状态”读的是仅在 preorder 完成时重建的旧缓存，所以必须退出再进入
  工坊才变化。补丁现在监听自定义原型重载，在状态面板下一次绘制前通过游戏自己的
  `get_io_states` 与托管复制/销毁函数重建输入、输出及第三组 IO 缓存；不会用裸内存复制破坏
  Nim 字符串的所有权。
- **修复工坊内改名后表格仍显示旧标签**：跨帧缓存以前拿“文字包围盒中心”识别引脚，改名会
  改变文字宽度，也就改变中心点，导致输入、输出引脚都可能与自己的缓存行失配。现在有白色
  引脚线段时以稳定的“引脚端点 + 朝向”匹配，只有首帧尚无白线数据时才回退到文字中心；同时
  表格延后到本帧最后一个引脚采集完毕后再画，直接使用本帧的新标签。只改标签不再需要退出并
  重进元件工坊，结构性增删引脚仍保留一帧的稳定期。
- **修好"自定义元件引脚名太长时在元件预览里糊成一团"**：预览（底部元件栏与元件工坊共用
  `build_custom_component_preview`，EXE RVA 0x38e990）按引脚逐个调用引擎导出的 `igText`
  画名字，而预览把元件缩小、名字却按原字号画，于是长名字压住旁边的名字（实测名字可到 470 px，
  而同一排引脚只隔 24 px）。摊开、倾斜 45°、缩小都试过，在 400 px 高的面板里只会让名字
  到处乱飞，最终按玩家的方案改成**编号表**：预览图里只画编号，名字全部放进面板空白一侧的
  表格里（多列排布，字号默认 70%，`TC_MODLOADER_PIN_TABLE=<百分比>` 可调，`=0` 关闭）。
  编号由补丁 Mod **实时分配**：先从名字反推出引脚位置，再按引脚外框**顺时针**走一圈
  （上 → 右 → 下 → 左，从左上角开始），编号递增。编号用纯白画（预览里的文字偏暗，
  一位数字要看得见）；每个编号**正对自己的引脚**、紧贴该引脚那条白线的外端（4 px 间距），
  同一条边上的编号在同一行（试过错开成两排换取更大字号，玩家反馈"看着错位、很乱"，
  因此对齐优先）；字号由"每条边的跨度 ÷ 该边引脚数"（不用最近邻，手绘元件可能有两个引脚
  几乎重合）算出的最小间距决定，朝外会顶到面板边时整条边改画在内侧。
  预览给每个引脚画的白条现在只用来标位置：加载器把它厚度减半、长度缩到一半，而且缩短时
  **固定贴近引脚的那一端**（早先按中心缩短，白条反而离引脚更远）。这根白条同时是"引脚在
  哪、在哪条边、朝外是哪边"的唯一精确依据——它的矩形被记进本帧的引脚记录，下一帧的编号
  就按它定位（钩 `ImDrawList_AddRectFilled`，只认预览那一处调用）。
  同一套逻辑也覆盖**元件工坊里"编辑外观"的编辑器**（`build_editor`，名字 0x3ae126、白条
  0x3adf96）：那里原先各画各的，引脚名照样重叠。编辑器里**只画编号、不画表格**——整面板都
  是游戏自己的调色板，表格会压住颜色（玩家反馈）；元件栏／工坊预览里的表格则优先放在面板
  **右侧**（左侧常有游戏自己的控件）。
  切换预览／外观编辑器时原本会闪一下原版的重叠名字：编号依据的是**完整一帧**的引脚数据，
  切换那一帧没有数据，旧实现"认不出就按原样画"。现在认不出上一帧的名字**这一帧不画**
  （编号下一帧出现），表格也延后一帧，切换变成干净的替换、中间最多空一帧（约 16 ms）；
  位置匹配放宽到 2 px，面板轻微滚动不会被误判成切换。
  同一份源码可以用 `-DTC_PIN_PATCH_ONLY` 编成**独立补丁**（`dist\pin-names-patch\game_engine.dll`
  ＋安装说明）：它照样是 game_engine.dll 的代理（转发全部导出、实现 igEnd），但**不扫描/加载
  任何 mod、不改存档目录、没有 Mods 菜单**，只装这三处钩子，给不想要加载器的玩家用；
  版本不符时它不装任何钩子，游戏等同原版。`build.ps1` 会一并产出这个补丁。
  启用补丁 Mod 后只接管预览那一次文本调用（返回地址 0x38f0ef），其余 193 个调用点原样转发
  （变参经 `igTextV`）；一切判断基于**上一帧**完整数据（`igEnd` 作为帧边界），因此编号和表格
  逐帧稳定；绘制用的小字号按窗口对象上的 `FontWindowScale`（`[window+0x308]`）精确还原，
  不再因 ImGui 的整数取整逐帧漂移（实测曾让面板文字从 29 px 慢慢缩到 28 px）。
  证据：改前 `build/pin-label-out/theirs9-preview.png`、放弃方案
  `theirs20-preview.png`、只给放不下的编号 `theirs26-preview.png`、最终
  `build/pin-label-out/theirs32-preview.png`；同时删掉了上一轮改错地方（棋盘标签网格）的
  45° 与那条会让游戏卡死的 0xe9100 死钩子。回归：
  `tests/native-component-playtest.ps1`、`tests/native.ps1`、`tests/ui-board-panel-playtest.ps1`、
  `tests/wire-palette-tool-playtest.ps1` 全部通过。
  调查与地址见 [research/pin-label-overlap.md](research/pin-label-overlap.md)。

## 未发布 — 关卡波形面板与 VCD／图片导出

- **修好"Mod 页面标题字号从未按主页字号显示"（一个静默失效的绑定）**：页面容器为了让标题
  与游戏主页同字号，会调用游戏自己的字号栈。但那两个函数（`igPushFontScale` /
  `igPopFontScale`）是**游戏可执行文件**里的 Nim 函数，代码却按名字去**引擎 DLL** 里找
  （引擎导出 1505 个名字里没有一个 `presenterZ…`），因此永远解析失败；而且 PopFontScale 的
  名字还写错了一位（真实是 `_u7966`，写的是 `_u7940`）。`if (withFont)` 永远为假 → 页面
  标题一直用窗口自带字号，且不报错。现在改为对**宿主自己的窗口**调用引擎导出
  `igSetWindowFontScale`（页头用主页倍率、内容区恢复 1.0），不再驱动游戏的字体栈——试过
  绑游戏那两个函数，结果是页面连绘制体都进不去（沙箱里 `-DriverMode` 直接失败），所以
  只用"我们自己的窗口、我们自己的缩放"。证据：日志
  `Mod page font scale applied: window font 45 -> 86`，且 `tests/ui-page-playtest.ps1`
  的驱动模式与双插件模式都通过。

- **修好"工具栏里的 Mod 工具展开后没有文字"**：根因不是字号——插件被注入时 ImGui 的当前
  字体是游戏的**图标字体** `Icon_Complete.ttf`，而图标字体没有拉丁/中日韩字形，
  `igTextUnformatted` 提交的字符串连一个字形顶点都不产生（矩形/按钮底不依赖字形，所以只有
  文字消失）。加载器现在启动时按符号读游戏的字体表
  （`defined_fonts__presenterZimguiZimgui_u7413`）挑出正文面（本构建
  `defined_fonts[2]=NoroshiCode_Regular.ttf`），在绘制插件工具前用游戏自己的
  `igPushFont__presenterZimguiZimgui_u7614` 推送、画完 `igPopFont()` 还原（与游戏在同一个
  函数里持有的字体作用域严格配对），并且**不再改写窗口字号**——借用正文字体后字号就是游戏
  自己的 UI 字号（实测 45 px，与侧栏面板一致），日志会写
  `Tool column text font: defined_fonts[2]=... (used for plugin tool draws)`。
  插件侧不需要任何字体处理：`tc::ui::text` 直接可显示中英文。
- 新增真机端到端用例 `tests/wire-palette-tool-playtest.ps1`（配
  `tests/toolbar-hover-driver.cpp`）：把真实鼠标压在调色盘瓦片上抓帧，得到展开面板里中英文
  正常显示的截图；沙箱准备按 `state.json` 的 `original` 哈希先还原
  `asset/shader/*.vert` 再 `apply`，避免二次打补丁把游戏打死。诊断工具
  `tests/toolbar-font-probe.cpp` / `tests/toolbar-font-playtest.ps1` 记录三个上下文的字体、
  游戏字体表枚举与推字体前后的截图对照；加载器新增开发用 `TC_MODLOADER_TRACE_TEXT=<子串>`
  （钩 `igRenderText`，打印字体/字号/顶点数增量）与 `tools/scan-calls.js`（从导出表反查
  调用点）。细节与反汇编证据见 [research/toolbar-tool-handoff.md](research/toolbar-tool-handoff.md)。

- **新增"工具栏插槽"能力，导线调色盘变成游戏里的一个工具**：宿主接口
  `tc::ui::registerBoardToolbar(id, draw, user, host)`（插槽类型 `TC_UI_SLOT_BOARD_TOOLBAR`）
  让插件把控件画进**游戏自己的工具栏**里——加载器在游戏最后一个工具按钮的子窗口结束时
  注入，并把插件工具摆到该按钮正下方（工具列几何是加载器从游戏自己的工具按钮实测学到的，
  不是写死的坐标）。因此调色盘现在是一个 40×40 的**色块按钮**，**鼠标悬停**才展开调色板
  弹窗（可视取色器：色相/饱和度方块 + 色相条 + 透明度条，带 R/G/B、H/S/V、#hex 精确输入，
  松手自动保存），另有 `取色器 / 使用此颜色`、游戏自带 11 色、我的颜色与最近用过。功能不变：
  仍然接管游戏调色板表、导线取色器、新导线/放置导线使用当前色，`palette.json` 存档照旧。
- 新增开发探针 `dev.enter-board`（`tests/enter-board.cpp`）：只 Hook `igInvisibleButton`，
  自动按一次主页入口进入关卡——板内界面（侧栏面板、电路）只能在关卡里看到，而其它驱动
  会占用 `handle_update_wire`，和调色盘冲突。配合下面的定时截图即可给板内界面截图。
- 加载器的开发截图增加**定时触发**：`TC_MODLOADER_SHOT` + `TC_MODLOADER_SHOT_DELAY=<毫秒>`
  在 `igEnd` 里按时间抓帧（菜单页的绘制只在主页运行，抓不到关卡里的东西）。
- **Mod 管理页重做**：从"一屏文字 + 单列列表"改成两栏式界面 —— 左侧包列表（勾选框 +
  名称/版本，启用的显示绿色、有问题的显示红色、当前选中行高亮），右侧是选中包的详情
  （名称、id/版本/作者、是否勾选、本次运行状态、原生/资源类型、资源与原生文件数、
  包文件名与 SHA、依赖、描述、错误信息）＋「加入/移出启用列表」「打开文件夹」；工具栏
  `刷新列表 / 打开文件夹 / 全部启用 / 全部停用`；存档设置收进可折叠的「存档（已隔离）」；
  页脚是未保存提示 +「应用更改 / 关闭」+ 一行状态。仍然只用引擎导出的 ImGui 接口，没有
  自绘控件。
- 加载器新增两个**开发用**环境变量（不设就不生效）：`TC_MODLOADER_OPEN=1` 让管理页
  在启动后自动打开，`TC_MODLOADER_SHOT=<文件.bmp>` 在管理页打开若干帧后用 `glReadPixels`
  把画面写成 BMP——这台机器的游戏窗口是独占翻转的全屏 GL 窗口，外部截图只能拿到黑屏，
  所以界面改动只能这样看。
- **工具栏插槽对多 Mod 是通用且无冲突的**：所有插件工具按 `Mod id → 插槽 id` 排序后
  依次绘制（顺序与加载顺序无关，可复现）；每个插槽在 `PushID(mod)` + `PushID(slot)` 的
  独立作用域里绘制，两个 Mod 用同名控件不会互相串；每个插件最多 8 个插槽，冲突的 slot_id
  会被拒绝（返回 −3）。日志给出布局：`Board tools in the game's tool column (top to bottom): …`。
- **工具瓦片按游戏自己的尺寸与网格画**：游戏工具按钮实测是 80×80、两列、列距/行距 96、
  底色 `(53,50,68)`、圆角 10；插件工具沿用同一套（加载器从游戏按钮学到网格间距并按格
  排布，插件用 `tc::ui::Canvas` 画同尺寸瓦片、底色与圆角，当前颜色作为"图标"占据游戏的
  图标区 44×44）。无绘图能力时回退为同尺寸的普通色块按钮。`colorPicker` 之类的大控件留在
  悬停弹窗里，不再占据工具栏。
- 悬停判定修正：弹窗会盖住瓦片，ImGui 的 item hover 随之变假，于是"关→开→关"逐帧闪烁。
  现在改成**几何判定**（鼠标是否落在瓦片矩形内，`Canvas::mousePosition()` 与尺寸比较），
  并把弹窗放在瓦片**旁边**而不是上面（`setNextWindowPos(..., Cond_Appearing)`，与游戏自己的
  工具弹窗一致）；鼠标同时不在瓦片和弹窗上（且没有控件在拖动）时才关闭，因此"移开即收起"
  不再抖动。
- **输入/输出数量改为按电路板数**：游戏的 `level_used_input` / `level_used_outputs`
  两个全局量不可信——实测在一个"1 进 1 出"的电路上它们仍返回 `2/2`，面板因此多画了两条
  泳道。现在插件直接数板上的 IO 元件（kind `0x3f` = 关卡输入、`0x44` = 关卡输出，与已验证
  的自定义逻辑网表构建器同一套），并通过 `Sampler::setDeclaredCounts()` 覆盖全局量；
  板上一个都没有时仍回退到全局量。日志会写一行 `Waveform: the board has N input(s) and
  M output(s)`。
- **重新开始仿真会自动清空波形**：检测到周期**倒退**（新一轮仿真）时清掉已有采样、从头
  记录，不再把两次运行的波形接在一起；日志写 `simulation restarted - waveform cleared`。
- **导线探针**：在电路板上选中一条导线 → 面板 **Probe wire** 把它加成一条绿色泳道
  （同时写进 VCD，变量名 `p0`…），**Clear probes** 清空。取值用游戏自己的
  `sim_state_read_bits(offset, width)`；导线记录 `+0x38` 就是状态字节偏移、`+0x30` 是位宽
  （实测 + 反汇编核对）。板模型由一个没人抢的 Hook 捕获：`load_level` 的第一个参数。
  真机断言：驱动器挑"接在关卡输出侧的那条导线"，逐行比较探针值与关卡自己的输出历史，
  必须完全一致（`and_gate` 的 `0,0,0,1`）。
- 采样时机修正：仿真状态是在步进**结束**时写好的，因此在"周期刚变化"的那帧读会拿到上一
  周期的值（实测关卡输出已为 1、探针仍为 0）。`Sampler::sample()` 现在把行**延后一次调用
  落盘**（捕获时读关卡 I/O，落盘时读探针），行为仍是"一周期一行、暂停不增长"。
- 波形只跟着**仿真**走，不再跟着渲染帧走：`tc::trace::Sampler::sample()` 只在周期变化时
  追加一行（暂停、关卡还没开始跑时一行都不加），因此面板头部现在是
  `samples N  simulating / paused (waveform held)  cycle a..b` —— 一帧一行变成了
  **一个周期一行**。代价写清楚：仿真跑得比渲染快（例如"连续运行"）时两次采样之间会跨过
  多个周期，波形在 VCD／界面上表现为时间戳跳跃；要让每个周期都被画出来需要挂每周期步进
  函数并在仿真线程缓冲，属于下一步（见 [verification.md](verification.md)）。
- 面板可以**指定监测哪些信号**：每条输入／输出一个复选框，未勾选的信号不再画、也不写进
  VCD（数据仍在采样，勾回来即可），带 `watching 2/2 in, 2/2 out` 的提示。顺序调整为
  波形画布在上、控件在下——控件排在画布前面时会把画布挤出宿主给的内容高度。
- 新增 `sdk/tc_trace.h`（`tc::trace::Sampler`）：只读地按周期取当前关卡的输入与输出，
  数据来源是游戏自己的两份历史缓冲（`simulation_input_replay`、
  `simulation_output_history_pins`），槽位**运行期按"哪些字节在动"发现**，数量以
  `TCGameStateModel` 声明的引脚数为准；某个引脚整段没变化时退回按相邻槽位间距推断，
  并用 `assumedStride()` 明说。`writeVcd()` 输出标准 VCD（GTKWave／Surfer 可直接打开）。
- 新增示例 `examples/waveform-demo`（`dist/example.waveform-demo.mod`）：在电路板侧栏面板里
  把关卡的每个输入／输出画成方波泳道（上蓝下黄、8 周期时间网格、只显示最近 N 个采样），
  带 **Export VCD**、**Export image**、**Clear** 三个按钮。示例包 `hooks=0`，面板完全由
  加载器调度，关卡关闭即不再绘制。
- 导出图片只能从进程内取：本构建是**独占翻转（independent flip）的置顶 GL 窗口**，
  `PrintWindow` 与桌面抓屏都拿不到任何像素（实测；仓库里既有的 UI 截图因此全是纯黑）。
  插件用 `glReadPixels` 读默认帧缓冲并写 32 位 BMP（`GetProcAddress("opengl32.dll")`
  取 GL 1.1 入口，不需要额外链接）；读取发生在 ImGui 帧构建期间，拿到的是**两帧前**的
  画面——面板每帧都在画，所以正是想要的那一张。
- 新增离线单测 `tests/trace.cpp`（`build.ps1` 内执行）：按实测布局（输入槽 0/8、输出槽
  55/64）验证槽位发现、数值、稀疏回退与 VCD 文本。
- 新增真机用例 `tests/waveform-playtest.ps1`（驱动 `tests/waveform-driver.hpp`，测试包
  `dev.waveform-demo-driver`）：驱动器自己进入 `and_gate`、编译、运行，插件记录面板真正
  画出的每一行，脚本断言它与关卡自带测试一致（输入 0,1,2,3 → 输出 0,0,0,1，输出为高的
  行必定输入为 3）、VCD 里带同样信号与数值，并在**加载器报出的面板矩形内**统计泳道颜色
  像素，证明波形确实出现在渲染帧里。`-Example` 模式只验证发行包注册与加载。
- 修正 `build.ps1` 里 menu-demo 三处 `-D...='"..."'`：Windows PowerShell 5.1（文档推荐的
  `powershell -File build.ps1` 用的就是它）会把原生参数里的双引号丢掉，`-D` 定义变成
  `settings` 而编译失败；带空格的引号定义改由 g++ 自己解析的响应文件传递，5.1 与
  PowerShell 7 下都能构建。
- `dev.cost-watch`（开发探针）不再 Hook `handle_update_wire`：Hook 是**独占**的，探针先
  拿到这个目标就会让真正需要它的玩家 Mod 加载失败（`local.wire-palette` 依赖同一个
  `handle_update_wire`，加载顺序一变就成败不定——日志里从 9/17 起成对出现）。探针只保留
  评分相关目标（`get_cost`／`get_delay_cost`／`get_gate_cost`／`build_scores`／`preorder`），
  报告里的 board 行随之固定为 `no context yet`；评分数字不依赖它。

## 未发布 — 自定义 UI 绘图

- 新增宿主可选接口 `TCHost::register_ui_slot`（尾部追加，旧宿主按结构大小返回 −1）与
  SDK `tc::ui::registerBoardPanel()`：把插件面板画进**电路板自己的窗口**右边缘。
  宿主拥有容器、ID 作用域、输入归属与生命周期，插件只画内容；每插件上限 8 个插槽，
  同名插槽／同名控件跨插件互不影响。
- 输入归属：宿主在游戏自己采样鼠标状态之前绘制面板（`build_board_ui` 的
  `igIsAnyItemActive` 调用点，RVA `0x46b593`），因此该帧 "有控件在活动" 的回答包含面板，
  落在面板上的点击／拖动不会被电路板同时处理。插件若也 Hook 了同一个入口（MinHook 会
  跟进 `jmp` thunk、换掉返回地址），加载器回退到一次短线栈回溯来确认调用点。
- 生命周期：面板由电路板自己的每帧代码绘制，关卡关闭即不再绘制，没有需要清理的状态。
  驱动测试用游戏自身的 `change_scene` 离开关卡，确认面板帧随之停止。
- 侧栏面板的内容区改为独立子窗口：内容被裁到面板内，被裁掉的项目不可悬停／点击；
  内容超出高度时由宿主在右缘绘制滚动条并驱动滚动（本构建的滚轮不会送到 ImGui，
  内容子窗口本身可滚动，插件也可自行调用 `igSetScrollY_Float`）。
- 主菜单页面容器改用同一段内容区实现（`drawContentRegion`／`scrollStrip`）：页面内容
  同样被裁剪、可滚动，坐标相对内容区；`tests/ui-page-playtest.ps1 -DriverMode` 断言
  内容区与滚动条存在且页面控件仍可点击。
- 面板只声明自己的矩形：面板外的点击仍归电路板，真机测试用"折叠线以下的探针点击"
  与画布点击做了双向对照。
- 新增 `examples/board-panel`（`dist/example.board-panel.mod`）：Ping 按钮、复选框、
  波形画布和一段 24 行、需要滚动才能看完的列表；新增
  `tests/ui-board-panel-playtest.ps1`（真实鼠标点击 + 游戏自身输入采样正对照 +
  裁剪探针 + 滚动条拖动 + 场景关闭）与 `tests/ui-slot.cpp`（宿主版本检查与参数校验）。
- 修正 `tools/exports.js`：改为从原版 `tc_game_engine.dll` 生成指纹与转发表。此前它从
  已安装的 `game_engine.dll`（即加载器）读取，会写出加载器永远无法匹配的引擎哈希，
  并把加载器自己实现的导出（`igEnd`、`igIsAnyItemActive`、`tc_logic_*`）误写成转发。
- 新增键盘支持与实测：`tc_ui.h` 增加 `Key_*` 枚举（ImGuiKey，值经真实按键消息核对）、
  `tc::ui::keys::down/pressed/released/amount`、`setKeyboardFocusHere()`、
  `isItemFocused()`；与既有的物理热键（`keyPressed(VK_*)`）在文档里区分开。
- 新增 `tests/ui-keyboard-playtest.ps1` 与探针包 `dev.ui-keyboard-probe`：用真实
  `WM_KEYDOWN/UP`（带扫描码）、`WM_CHAR`、`WM_IME_CHAR` 驱动插件页面，逐段断言缓冲区
  **完整内容**——按键打字一次一个字符、单独的 `WM_CHAR` 一次一个字符、`你`/`好`
  以 UTF-8 进入、Escape 被输入框消费并回滚文本。中文组合串／候选窗给出人工验收清单。
- 并入一条被纠正的误判：第一版驱动同时发 `WM_KEYDOWN` 与 `WM_CHAR`，每字符出现两次，
  曾写成"这套构建重复投递字符"。实际是消息循环的 `TranslateMessage` 已把按键转成
  `WM_CHAR`，多发的才是重复来源；`tc_ui.h` 与文档已按实测更正，不需要插件去重。
- 新增 `tests/ui-key.cpp`：键盘辅助函数的严格解析、未加载时的空操作、参数转发与枚举值。
- 新增一键人工验收台 `tests/manual-ime-test.ps1`：准备隔离沙箱、写出 `CHECKLIST.txt`、
  以可见窗口启动游戏（`-Hidden` 只做冒烟检查）；探针同时记录系统为输入法组合窗／候选窗
  要求的位置（`IME page composition=… candidate=…`），让"候选窗位置对不对"变成日志里的数字。
- 人工验收（2026-09-18，用户确认）：页面与板上面板的中文输入、选词上屏、Esc 取消组合
  通过；候选窗跟随光标（`composition/candidate style=0x32` 随每个字移动）；板上打字未
  触发板面快捷键。已知现象：独占全屏下候选窗出现时画面闪一下，属系统合成行为，加载器
  不绘制候选窗；文档给出可选缓解方式。
- 显示诊断与窗口尺寸回归：每次启动记录 `Display: dpi-awareness=… window-dpi=… monitors=…
  client=… surface=… ratio=…`；`tests/ui-board-panel-playtest.ps1` 会把游戏窗口从
  2560x1600 改成 1792x1120，断言面板按新窗口重排、插件上报的按钮绝对坐标随之变化，且
  真实点击仍然命中（`Ping clicked, count=2`）。本机 175% 缩放（`window-dpi=168`）下
  全部通过。
- 内容子区域不再保留引擎自己那条不可交互的滚动条，插件获得完整内容宽度；宿主的滚动条
  仍是唯一滚动入口。
- 面板新增宿主绘制的**折叠开关**（标题行右侧 `-`/`+`，整块可点）：折叠后只剩一条标题栏，
  再点展开。状态由宿主保存，插件无法自己折叠或留下无法恢复的状态；折叠期间裁剪区只有
  标题栏高，内容既不显示也不可点，但插件 `draw()` 仍会被调用。
  `tests/ui-board-panel-playtest.ps1` 断言：折叠后矩形高 60、折叠期间点按钮不计数、
  展开后按钮恢复（`count=2`），且折叠期间插件仍在绘制。

- 新增 `tc_ui_texture.h` 和宿主可选纹理 API：包内图片（中文路径）、RGBA 上传、UV 裁切／
  翻转／着色和图片按钮。按 Mod 管理所有权与配额，延迟到后续帧释放，上传后恢复 GL 状态。
- PNG／RGBA 真实 GPU 回读、透明度／方向、UV、错误输入、跨 Mod 释放拒绝、线程限制、
  配额与连续 180 帧绘制已通过。示例加入加载／释放／翻转图片。

- 新增可选 `tc_ui_draw.h`，独立加载 19 个绘图／几何导出，不改变宿主 ABI 和旧控件加载要求。
- `Canvas` 提供线、圆角矩形、渐变、圆、三角形、贝塞尔、折线、凸／凹多边形和文字；
  统一局部坐标，自动占位、鼠标状态快照，画布和嵌套裁剪作用域自动恢复。
- 新增 `example.drawing-demo.mod`：主菜单页面与 F8 浮动面板，支持拖动曲线控制点。
- 真实引擎逐图元顶点回归、颜色／坐标回读、移动／缩放／子窗口滚动、裁剪恢复与 180 帧持续运行。
- 更正旧交接中“插件不能调用 draw list”的推断；旧探针失败不代表该能力不可用。

## 0.4.6 — 声明式 C++ 元件

- 新增 `TCHost::register_component` 和 `tc::TCNativeComponent`：只声明稳定 ID、名称、
  输入／输出引脚、代价和回调，加载器自动生成定义并完成导入、原型设置和逻辑注册。
- 输入采集顺序与声明顺序一致，自动建立依赖链，支持每方向 1–8 脚，不要求作者写占位门。
- 拒绝重复 ID、非法形状和超界代价；旧宿主通过结构大小检查返回不可用。
- `examples/byte-adder` 移除对手写定义生成器的依赖，仍保留 1 门／1 延迟声明。
- 修正输入跨 payload 字边界时的布局，避免总位宽不超 128 却因空隙访问第三个字。
- 新增声明接口单元测试和 8 输入／8 输出真机压力用例。

## 0.4.5 — 插件界面接口

- 新增 `sdk/tc_ui.h`：插件画面板需要的东西一次封装好——45 个引擎 ImGui 导出在加载期
  统一解析（缺任何一个就写日志并拒绝加载，不再等到绘图时才炸）、类型化控件、
  `Window`/`Child`/`Disabled`/`TextWrap` RAII 作用域、面板定位与字号约定、视口尺寸、
  边沿触发热键。纯头文件，不改 ABI：全部走已有的 `host->engine_proc`。
- 修正 `examples/wire-palette` 的一个真实缺陷：`igSetNextWindowPos` 实际是三个参数
  （`pos`/`cond`/`pivot`，引擎从第三个参数寄存器读 pivot），原来只传两个，pivot 取的是
  寄存器里的残留值。
- 三个界面示例改用 `tc_ui.h`：`mod-inspector`（面板 + 视口 + 鼠标坐标）、
  `cycle-guard`（工具条 + 面板 + 复选框 + 禁用/换行作用域，并补上清单里一直写着、
  代码里却并不存在的 F6 热键）、`wire-palette`（取色器、颜色块、输入项状态）。
- 新增 `tests/ui-playtest.ps1`：在隔离副本里跑真实 `tcmod.mod-inspector` 包，断言面板确实
  在真实 ImGui 帧内绘制，并核对视口/鼠标返回值合理；证据行会打印实际分辨率。
- 修正 `tests/native.ps1` 的既有缺陷：夹具没有拷贝 `compile.dll`，native 运行时初始化
  必然抛错，四个模式全部失败；补上后四项通过。

## 0.4.4 — 界面刷新阶段的计算

- 修掉 `examples/byte-adder` 的一个真实缺陷：回调在 `TC_LOGIC_REFRESH`（界面刷新）
  阶段直接返回，没有算输出。仿真/关卡判定走 `TC_LOGIC_CYCLE` 所以一直是对的，
  但界面与元件工坊的实时表格读的是刷新阶段的值，于是加法元件"不会算加法"
  （用户实测反馈）。
- 现在 `CYCLE` 与 `REFRESH` 共用 `computeBytes()`，只有 `REFRESH` 跳过计数与状态
  （刷新跑在状态副本上）。日志新增前几次刷新的 `byte-adder: peek ...` 取样。
- 新增 `tests/byte-adder-smoke.ps1`：隔离副本里跑**真实包**，断言引脚几何、
  刷新取样、周期取样都满足 `sum/carry_out` 的预期函数，且关卡 40 周期无失配。
  此前这条路径只有仿真断言，缺陷因此漏网。

## 0.4.3 — 声明门数进入板级分数

- 整板门数原先无视声明值：游戏会把自定义元件展开成内部电路再逐门相加，
  `examples/byte-adder`（声明 1 门，占位电路 Mux+NOT）因此在界面显示 51 门。
- 加载器现在在插件自己的 `tc_mod_load` 作用域内捕获原型上的 `(门数, 延迟)`，
  并用声明门数替换该元件的展开计数；同一元件展开出的内部节点不再重复计数，
  嵌套 Mod 只计最外层一次。游戏自身「跳过自定义元件」的调用语义不变。
- 真机回归：`tests/component-cost-playtest.ps1` 新增 `TC_DECLARE_GATES` /
  `TC_DECLARE_DELAY` 与 `observe` 模式（元件由真实 Mod 提供），byte-adder 关卡
  编译统计与界面分数均为 `gates=1 delay=1`；`tests/component-timing-playtest.ps1`
  9 例与 `tests/native-logic-playtest.ps1` 11 例全绿。
- `dev.cost-watch` 改为按目标逐个安装 Hook：被加载器占用的
  `get_gate_cost` / `preorder` 会跳过并写明（`observing 4/6 targets`），
  其余观察照常工作，不再整体加载失败。

## 0.4.2 — 原生逻辑：多位宽与形状组合

- ABI 与运行时按引脚位宽泛化：注册接受每方向 ≤8 脚、每脚 1–64 位、总输入 ≤128 位。
- 输入按引脚宽度打包进两个 payload 字，保持 4 参数外调形状（超过 4 个参数会让游戏 JIT
  触发 `register_frame.nim(119,3) ... == EXP_NULL`）。
- 字宽输出改为"回调写预留状态槽 + 生成代码按位 `load` 组字"，因为外调返回值只有按 1 位
  消费才可靠；形式与游戏 `com_maker_bit_8` 一致。
- 内部门识别范围扩到 `0x03`–`0x0b`、`0x12`–`0x1e` 与字宽 Mux `0x2a`；未识别实例写诊断。
- 真机场景扩到 11 个：新增 2×8 位、3×8 位、8+3 位、1+8+8 → 8+1 位四类形状。
- 修正夹具两处缺陷（组件计数不符、连线少走一格），并新增 `exposes N operand(s)` 诊断，
  会原样打印退化的发射行。

## 0.4.1 — 原生逻辑：接口形状自定义

- 回调 ABI 升到 2：`TCLogicIO` 携带 `input_count`/`output_count`、`inputs[8]`、`outputs[8]`。
- 形状由元件定义决定；回调输入元组 = 第一个输出门的操作数列表；一次 invoke + 多次回读。
- 新增 1 进 1 出、3 进 1 出、3 进 2 出三个真机场景（`not_gate`/`and_gate_3`/`full_adder`）。
- 确认两条定义序列化约束：多脚引脚几何（左列间距 8）与 v14 组件尾部字段顺序。
- 修正夹具缺陷：原理图不再复制关卡自带 IO（否则关卡判定与界面表格读不同输出脚，
  会出现"当前输出与期望不符却通关"）。

## 0.4.0 — 元件时序、声明延迟与原生逻辑（分发包版本）

- 电路封装元件的**声明延迟**参与编译期时序统计：串联相加、并联取最长、嵌套只计外层一次；
  反馈环、多驱动、收缩后成环、非法数据与溢出保留游戏原生统计并写日志。
- SDK 新增 `setPrototypeGateCost` / `setPrototypeDelay` / `TCPrototypeBuilder::setDesignCost`。
- 新增 `dev.cost-watch.mod` 诊断包、`design-stats.txt` 覆盖开关与
  `python tools/circuit_format.py --analyze` 设计期预检。
- 原生逻辑回调（`sdk/tc_logic_api.h` + `register_logic`）进入真实代码生成链路，
  游戏继续负责关卡判定、内置元件、暂停与重置；`examples/custom-or` 演示内部 AND 后备、
  回调 OR。
- 真机回归：`tests/component-timing-playtest.ps1` 9 例；`tests/native-logic-playtest.ps1`
  3 例（`or`/`mixed`/`multi`）。
- 新增元件探测与验证工具：`dev.kind-list.mod`（125 个内置元件 kind/引脚）、
  Delay Line 引脚与代价反推、`delay` 拓扑 fixture。

## 0.3.0 — 独立存档与升级

- 装有加载器的游戏始终使用独立存档（即使全部停用或 Shift 跳过原生插件）。
- 新增"导入原版存档（新副本）"：每次新建副本、逐文件校验、失败不切换；排除 Steam 云元数据。
- 存档路径在游戏初始化前重定向，按 `USERPROFILE` 拼接（仅设 `APPDATA` 不足以隔离）。
- 副本选择保存在 `tc-modloader-data/saves.ini`。
- 验证：Unicode 文件名、二进制电路原样复制、重复导入生成不同副本、符号链接/目录联接拒绝、
  原版测试源 464 个文件 SHA-256 一致。

## 0.2.0 — 原生插件运行时

- 原生 DLL 加载、`resolve_symbol`、Hook 生命周期（创建禁用、成功后才启用、失败移除）。
- `sim_do` 目标周期拦截示例 `example.cycle-guard.mod`：把连续运行限制为当前周期 + N。
- 支持使用游戏自身 ImGui 绘制插件面板；`on_frame` 每 UI 帧一次。
- 自动验证：25 项包管理集成测试、6 项安装器测试、原生 Hook 正常/篡改/冲突/停用测试。
- 实机：加载独立 DLL、安装 `sim_do` Hook、绘制中文面板、向真实仿真内核提交目标周期并
  读取周期达到 99（SELFTEST）。

## 0.1.0 — 资源 Mod 与 Mods 管理页

- `.mod` 资源包（`format: 1`）：`files/` 文件替换/新增、精确文本补丁、停用恢复、
  大小写与路径冲突检查、事务回滚。
- 主菜单 Mods 入口与中文管理页：启用/应用/停用，区分“下次启动启用”与“本次运行中”。
- 安装器：安装、升级、卸载精确恢复、不兼容 EXE/引擎拒绝。

## 未发布内容说明

- 版本号、安装器与分发包仍为 0.4.0；0.4.1–0.4.6 的功能在源码树中可用，
  需要用 `build.ps1` 与 `package.ps1` 重新打包后才会进入玩家分发包。
- `sdk/tc_custom_logic.h`（整板解释器）在 0.4.1 起标记弃用，仅保留给研究探针。
- 路线决定：手写定义并登记版本 2 回调的元件，其内部电路只用一层占位门（每个输出脚一个可识别门、
  操作数即元件输入脚）。内部中间层不在加载器范围内，计划以后作为独立 Mod 提供；
  纯电路封装元件不受此限，多级内部电路照常由游戏展开。
