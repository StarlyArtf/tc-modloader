# TC-Verilog 内核

`sdk/tc_verilog.hpp` + `sdk/tc_verilog.cpp` 是面向 `TCLogicIOV3` 的第一版可综合
Verilog 内核。它不是把 Icarus Verilog 嵌进游戏，也不运行任意 testbench；源码在 Mod
加载时编译成一棵不可变的执行计划，每个元件实例只把寄存器、RAM 和时钟历史放在宿主提供的
`state` 中。因此同一个 `Program` 可以安全地供所有实例共享，存档、重置和 `REFRESH` 隔离仍由
加载器管理。

## 当前可写的子集

- 单个 ANSI 风格 `module`，端口为 `input` / `output`，宽度 1～64 位；
- `wire`、`reg`、`logic`，以及 `reg [W-1:0] mem [0:N]` 形式的定长数组；
- `assign`、括号、三目运算和常用算术／位运算／比较运算；
- 拼接 `{a, b}`、复制 `{N{value}}`（`N` 必须是常量）、位选择 `x[i]`、部分选择 `x[msb:lsb]`
  （边界必须是常量）与 `x[base +: width]` / `x[base -: width]`（宽度是常量、基准可以是变量）；
- `always` / `always_ff` 配边沿敏感表：`@(posedge clk)`、`@(negedge clk)`，以及异步复位常用的
  多事件表 `@(posedge clk or negedge rst_n)`（`,` 与 `or` 等价）；
- **组合式块**：`always_comb`、`always @(*)` / `always @*`、以及电平敏感表
  `always @(a or b)`。组合块里用**阻塞赋值 `=`**（时钟块里必须用 `<=`，两边混用会被拒绝），
  它不占敏感表状态字。编译器为组合块建立读写图：无环部分按拓扑序只执行一次，真正的组合环才保留
  最多 8 轮的不动点迭代，所以一个块读另一个块的结果与声明顺序无关。单一写者、所有路径完整赋值、
  无反馈的 `reg` 是本次调用的临时 scratch，不进入实例持久状态；不完整赋值（锁存语义）、多写者和
  环内变量仍保留状态字。CYCLE 在提交时钟块前先求值其组合依赖，提交后公开输出同一拍看到新值；
- `begin/end`、`if/else`、`case` / `casez` / `casex` 和非阻塞赋值 `<=`；
- `integer` / `genvar` 声明的**编译期循环变量**，以及 `for` 循环：语句里的
  `for (i = 0; i < N; i = i + 1) q[i] <= a[N-1-i];` 与模块级（`generate` 里或直接写）
  `for (...) assign y[i*4 +: 4] = ...;`。循环在编译期展开，循环变量在展开时折成字面量，
  所以它可以用在位选、部分选择、`[base +: width]` 的基准和任意算术里，且不占状态 word；
- **`parameter` / `localparam`**：模块头里的 `#(parameter W = 8, D = 4)`（`parameter` 关键字
  可省）与模块体里的 `parameter P = ...;` / `localparam P = ...;`，声明处即求值，可选
  `integer`/`signed` 或 `[range]` 类型被接受但忽略。常量可以用在**端口与信号位宽**
  （`input wire [W-1:0] a`）、数组上下界（`reg [W-1:0] mem [0:D-1]`）、部分选择边界、
  复制计数、循环边界和任意表达式里；它们和循环变量一样在编译期折成字面量，不占状态 word；
- 0、1、X、Z 常量。X/Z 在内核中分开保存；V3 没有 X，所以输出的 X 位会释放成 Z。
- **有符号算术**：端口/信号可以写 `signed`（`input wire signed [3:0] a`、`reg signed [7:0] q`），
  字面量可以写 `'s`（`8'shff`、`4'sd2`，也支持系统函数 `$signed(x)` / `$unsigned(x)`）。
  加减乘除、取模与 `<` `<=` `>` `>=` 在**两侧都是 signed** 时按有符号解释；位宽放大时 signed
  值按最高位符号扩展（X/Z 的最高位也照样扩展成 X/Z）；`>>>` / `<<<` 是算术/普通左移，其中
  `>>>` 在左值是 signed 时补符号位，`>>` 永远补 0。按 Verilog 的规则，只要有一个操作数不是
  signed，整个表达式就按无符号算；比较、拼接、位选和部分选择的结果都是无符号；`==`/`!=`
  只看位模式，signed 与否不影响结果。
- **模块实例化**：一个文件可以写多个 `module`，`<模块名> [#(.P(v))] <实例名> (连接…) ;`
  把子模块**在编译期展开**进父模块（子模块的寄存器/存储器并入父模块的状态数组，端口连接变成
  驱动）。连接的四种写法都支持：具名 `.a(expr)`、具名简写 `.a`（接同名网）、空连接 `.a()` /
  位置空槽 `(a, , y)`（该端口不驱动 → 输入读作 Z）、以及位置连接 `(expr, expr)`；输出端口可以
  接整根 wire，也可以接**常量边界的位选/部分选择**（`y[g]`、`y[1:0]`），所以
  `generate for (g = 0; g < W; g = g + 1) leaf u(.a(a[g]), .y(y[g]));` 这种"每位一个实例"
  的写法成立。参数覆盖只支持具名形式 `#(.N(8))`（同一个子模块可以在不同实例上用不同参数）。
  顶层是**最后一个没有被任何模块实例化的 module**；每个实例都是重新解析一次子模块的 token
  区间，所以嵌套实例与逐实例参数覆盖走同一条路径；实例名就是层级路径的第一段（见下）。
- **层级名字（只读）**：可以用实例路径读被展开实例内部的信号，比如 `assign y = u0.sig;`、
  嵌套实例 `u0.u1.sig`，以及 generate 循环里的实例 `u0[1].sig`（同名实例的第 0 个也可以写成
  `u0.sig`）。它只能出现在表达式右边——给层级名字赋值会被拒绝（`a hierarchical name may only be
  read, not assigned to`）；本地同名信号优先（`u0` 若也是本模块的信号，`u0.sig` 会被当成它的
  位选之类，而不是层级路径）。`begin : name` 的块名仍然只是装饰，不参与路径。
- **generate 的 `if` / `case` 形式**：`if (<常量>) <生成块> [else …]`（含 `else if` 链）与
  `case`/`casez`/`casex (<常量>) … endcase`。条件与标签必须是常量（用参数最典型）；**所有分支
  都会被做语法检查，只有被选中的那一支进入设计**——所以两个分支可以声明同名信号、驱动同一根
  net，互不冲突（它们在编译期是互斥的）。`default` 写在哪一条都一样，只在没有标签命中时生效。

时钟块先读取同一份旧状态，再统一提交非阻塞赋值。这使同一拍里的寄存器链和 RAM 读写具备
NBA 语义。对 X/Z 数据、未知地址或越界地址的寄存器/RAM 写入会被跳过，不会偷偷写成零。
敏感表的每个条目各占一个状态 word 记录上一拍电平，任何条目出现配置的边沿就执行块体
（`posedge`：上一拍不是 1、这一拍是 1；`negedge`：上一拍不是 0、这一拍是 0——和 Verilog
把 x→1、x→0 也算边沿一致）。所以 `always @(posedge clk or negedge rst_n)` 里的复位是
**异步**的：`rst_n` 拉低那一下不需要时钟沿就会执行块体；复位保持期间每个时钟沿仍走复位分支；
而复位释放（表里没有 `posedge rst_n`）本身不触发。注意内核每个宿主周期只采样一次电平，
是“边沿检测”而不是事件队列，所以异步复位在下一个宿主周期生效。
位选与部分选择在赋值左侧同样可用（`q[0] <= a[7]; q[7:4] <= a[3:0];`），同一拍里写同一个
寄存器的多条非阻塞赋值按书写顺序合并，各自只拥有自己那几位。读到的下标越界时该位是 X
（跨界选出的位同样如此），写目标越界则整次写入跳过——和 RAM 地址越界的策略一致。

`case` 的比较是四态的：普通 `case` 逐位精确比较（X 只匹配 X、Z 只匹配 Z），`casez` 把标签里
的 `?`/`z`（`?` 在字面量里存成 X，所以标签的 X 位也算通配）和被测值里的 Z 当“不关心”，
`casex` 再额外把被测值里的 X 也当通配。标签可以写逗号列表（`3'd0, 3'd1:`），
`default` 可以出现在任意位置、且只在没有任何标签命中时才执行；被测值含未知位时，普通
`case` 会落到 `default`。

循环的边界（起值、条件、步进）必须是**常量**，内核按顺序展开：`for` 的条数上限 4096
（写成死循环会在这里报错），`generate` 循环至少要跑一次（语句循环可以跑零次）；
展开体里的循环变量不能逃出它自己的循环。`for` 头里就地声明（SystemVerilog 的
`for (genvar i = ...)` / `for (int i = ...)`）不支持，先写 `integer i;` 或 `genvar i;`；
`if` / `case` 形式的 generate 和带名字的 generate 块（`begin : name` 里的层级名）也还没有，
带名字的块可以写、但名字不参与任何引用。

**作用域**：每个展开的循环迭代、以及 `generate` 里的每个 `begin/end` 块都是独立作用域——
同一名字可以出现在不同迭代里（互不相干的信号），块内声明会**遮蔽**同名的外层信号，出了块就
看不到（引用会报未知信号）；同一个块里重复声明同名还是重复声明错误。于是"每个实例一份内部
连线/寄存器"可以写出来，例如生成体里 `wire t; sub u(.a(a[g]), .y(t)); assign y[g] = t;`
或 `reg r; always @(posedge clk) r <= d[g];`，每个迭代各有自己的线与寄存器（各自的状态字）。

参数在**声明处**求值，所以只能用在此之前声明的常量（前向引用报未知信号）；这一版没有模块
`defparam` 也不支持；`parameter` 可以被实例的具名覆盖（`#(.P(v))`）改掉，
`localparam` 不能（写了 `#(.P(v))` 覆盖它会被当成"该模块没有这个参数"）。类型/位宽标注只
影响可读性，常量本身按无符号值参与折叠。

实例化的边界：`inout` 端口不支持；输出端口只能接到 wire（或它的常量位选/部分选择），不能接到
`reg`；层级名字只能读、不能写；实例数组只通过 `generate for` 表达；位置形式的参数覆盖
（`#(8)`）不接受，要写 `#(.W(8))`；递归实例化会被深度上限（24 层）拒绝。

组合块的边界：同一个变量不能既被组合块又被时钟块赋值（解析时报
`'x' is assigned from both a combinational and a clocked block`）；`always_latch` 不支持
（不完整组合赋值仍按既有锁存状态保留）；组合结果里出现 X/Z 时那**整条赋值会被跳过**（状态字存不下 X——
需要三态/未知传递请用 `assign` 驱动 wire，输出脚会按 Z 释放）；组合块之间成环时取不动点
迭代 8 轮后的最后一个值（Verilog 会给 X）。

暂不支持 `inout`、`always_latch`、函数／任务、`initial`、延迟和 testbench
系统任务；也不支持拼接写在赋值左侧、复制计数或 `[msb:lsb]` 边界不是常量、
以及超过 64 位的拼接/复制。一条 wire 可以由多条 `assign` 驱动，只要它们的位段不重叠
（重叠会按“该位已有连续驱动”拒绝，和以前一样）。编译器会在首个未支持语法处返回行列号，
不会“尽量猜”。游戏当前的 word Switch 仍只能整脚驱动或整脚 Z；若一个多位结果只有部分位已知，
V3 桥会按整脚 Z 处理。

## 逐输出的组合依赖（为什么单周期 CPU 不会被判成环）

生成的元件在游戏里是一张**内壳电路**：编译器现在为每个输出单独算一份"实际读了哪些输入引脚"的
掩码，加载器按这份掩码给每个输出生成**自己的**依赖锥（输入收集器 → 该输出的依赖门 → 驱动），
而不是把每个输出都挂在一条吃全部输入的链上。注册日志里的 `deps=` 就是这张表。

单周期 RV32I 加游戏内置寄存器堆是最典型的例子：

```verilog
assign rs1_addr = instr[19:15];
assign rs2_addr = instr[24:20];
assign rd_addr  = instr[11:7];
assign rd_wdata = /* … 会读 rs1_data/rs2_data/dmem_rdata … */;
```

`rs1_addr` 只依赖 `instr`，所以游戏看到的是
`instr → 读地址 → 寄存器堆（时序写入口）`，而寄存器堆的读数据只喂给 ALU/写回，**不回到读地址**；
`dmem_addr` 同理不含 `dmem_rdata`。假环消失，仍然是单周期，不需要插 Delay。

带 `always @(posedge …)` / `negedge` 的模块也使用这张表。公开输出在 REFRESH 中按各自掩码求值；
加载器另建一个没有外部输出的隐藏全输入汇点，在 CYCLE 中恰好调用一次来提交时序状态。隐藏汇点
不会给任何公开输出增加组合边，所以内部 PC/寄存器堆既能正常更新，又不会重新引入假环。注册日志
会额外显示 `tick=hidden`。完整真机证据见
[../verification.md](../verification.md) 的"时钟模块：精确输出锥 + 隐藏 tick"一节。想离线看任意
模块的表：

桥接层还会把当前公开输出编号交给 TC-Verilog。内核使用编译期生成的反向切片，只执行该输出需要的
组合块并只序列化这一只输出；它不会再为 9 个 RV32I 输出把完整 AST 重跑 9 次。直接调用
`Program::execute` 时 `TCLogicIOV3::reserved=0` 仍表示求值全部输出，保持原有 SDK 行为；非零选择值
仅是加载器与 TC-Verilog 之间的私有约定。

同一实例的连续输出回调还会复用已经算出的**纯 scratch 组合块**。缓存按每个块实际读取的输入／状态
叶子单独校验，而不是比较整个组件输入：因此精确依赖桥在不同输出上把无关引脚置为 Z 时，公共的
`instr` 解码前驱仍能命中。命中键在实例缓冲中原位比较，避免每块分配临时向量；写锁存器、存储器
或架构状态的块不进入缓存，组合环也不进入缓存。

依赖掩码完全相同的输出会进一步合成一个回调组：第一个输出求值并发布整组，后续输出只读已发布值。
例如内置 PC/寄存器堆版 RV32I 的 9 个输出只有 `0x0` 与 `0x2` 两组，所以每拍由原来的“9 次公开
输出 + tick”降为“2 次公开组 + tick”。该能力由 `TC_COMPONENT_TYPE_GROUPED_OUTPUTS` 显式启用，
普通 V2/V3 插件保持逐输出行为。隐藏 tick 先执行仅含敏感信号的边沿计划；没有触发边沿就直接返回，
只有触发后才执行时钟 RHS 前驱和边沿提交。

### 纯组合模块：直接生成游戏 JIT 表达式

若模块满足 `state_words == 0`、没有时钟块，并且输出能用确定的二态组合逻辑完整求值，TC-Verilog
会进一步生成 `TC_COMPONENT_TYPE_INLINE_EXPRESSIONS`。加载器不再为这类输出调用 DLL：它把
Verilog 降成游戏编译器自己的 `U64` SSA，直接嵌入 CYCLE/REFRESH 程序。输入收集、跨 DLL FFI、
`TCLogicIOV3` 打包和运行时 AST 解释全部离开热路径；注册日志会显示 `jit=inline`。

生成器先按相同依赖掩码共享符号求值，再对整组做反向可达裁剪。加载器在游戏实际调度到组内第一个
输出时发射一次公共前缀，其余输出只发射自己的最终表达式；因此游戏无论怎样重排内部驱动，都不会
跨输出“先使用后声明”。裁剪后的程序还会逐项核对 `@N@` 输入只来自该输出公开的依赖掩码。
降低过程会折叠常量与布尔 Mux、消除整宽单驱动 wire/无符号扩宽别名，并省去可证明冗余的位宽掩码。
所有依赖组来自同一张规范 SSA 图；加载器按游戏真实调度顺序逐声明去重，所以不同掩码的输出也能
共享只依赖其公共输入的节点，同时仍在额外输入可用之后才发射后继。`rv32i_flat.v` 当前生成 13 路
程序，核内零状态；通用动态 Mux 使用 JIT 条件赋值，常量门控使用按位短公式，单比特复制和常量
拼接会直接折叠。永久驱动的 inline 输出还会绕过通用三态 Switch 的 OR/AND 合并、冲突检查与 Z
比较，CYCLE 路径直接发布值。当前注册文本约 138 KiB，沙盒生成代码为 117,012 B、690 个 SSA 临时量，且热
路径中没有 `tc_logic_*` 回调。

这个优化是保守的。存储器、锁存、组合环、X/Z、部分左值写入、除零可能性以及尚未覆盖的 signed
运算会让整个模块自动回退到上面的 V3 回调路径，语义不会为了速度被静默改写。inline 程序已经成为
游戏生成代码的一部分，所以修改它需要重新注册/重启；回调型且形状不变的模块仍可热更新。

```
g++ -std=c++17 -O2 -Isdk tools/verilog-dependencies.cpp sdk/tc_verilog.cpp -o build/verilog-dependencies.exe
build/verilog-dependencies.exe tests/fixtures/rv32i_single_cycle.v
```

## 磁盘项目与持久工作区

`sdk/tc_verilog_project.hpp` + `sdk/tc_verilog_project.cpp` 把多个 `.v` 文件组织成一个项目。
新项目使用 `format: 2`：

```json
{
  "format": 2,
  "project": {
    "id": "my-verilog-project",
    "name": "My Verilog Project",
    "description": "Components maintained in the in-game IDE",
    "source_roots": ["rtl"],
    "default_component": "ram16x8"
  },
  "build": {
    "max_source_bytes": 262144,
    "max_tokens": 65536,
    "max_state_words": 65536,
    "max_expression_depth": 128
  },
  "components": [
    {
      "key": "ram16x8",
      "source": "rtl/ram16x8.v",
      "top_module": "ram16x8",
      "group": "Memory",
      "enabled": true,
      "custom_id": "0x54435652414d3031",
      "type_id": "my-mod/ram16x8",
      "name": "RAM 16x8",
      "description": "Synchronous-write RAM",
      "gate_cost": 16,
      "delay": 1,
      "pin_lane": 3.0,
      "show_description": false
    }
  ]
}
```

`project.id` 和组件 `key` 是稳定 ID；改文件名不会改变组件 `key`。`default_component` 必须引用一个
已启用组件，`source_roots` 同时定义 IDE 文件树根和允许存放源码的范围。`top_module` 固定该目标应编译
出的顶层模块；`group` 是 IDE 中的逻辑分组；`enabled: false` 会保留文件和编辑能力，但跳过游戏注册。
`build` 可以收紧项目的源码、token、状态字和表达式深度上限，但不能突破调用方传入的
`CompileOptions` 安全上限。

`custom_id` 可以是 JSON 无符号整数或十进制／十六进制字符串；推荐字符串，避免其他 JSON 工具
把 64 位整数经过浮点数。`source` 必须是以 `/` 分隔的安全相对 `.v` 路径，绝对路径、`..`、
反斜杠和未知字段都会被拒绝。清单最多 1 MiB、256 个元件。旧的 `format: 1` 清单仍可直接加载，
文件管理操作也会继续按 format 1 回写，不会在用户不知情时自动迁移。

```cpp
static tc::verilog::Project project;

std::vector<tc::verilog::ProjectDiagnostic> errors;
if (!project.loadAdjacent("tc-verilog.json", &errors)) {
    // formatDiagnostic(errors[0]) 含阶段、文件、元件 ID、行列和原因
    return 1;
}
if (project.registerAll(host, &errors) != TC_COMPONENT_TYPES_OK) return 2;
```

`loadAdjacent` 使用包含 `tc_verilog_project.cpp` 的 DLL 路径，所以清单和 `.v` 可以放在 Mod 的
`native/` 下，与 DLL 一起进入加载器的包缓存。解析、读文件和编译全部成功后才替换旧项目；注册前
不会产生半成品元件。

需要让玩家修改源码时，先加载包内默认项目，再调用 `bootstrapWorkspace(data_directory/tc-verilog)`。
它只在工作区清单不存在时复制默认文件，已有源码绝不覆盖，并最后原子写入清单作为完成标记。
随后从工作区清单 `load` 并注册。编辑器调用 `validateSource` 做只读试编译，调用
`validateAndSaveSource` 时则先编译、成功后才用临时文件原子替换 `.v`；失败不会触碰磁盘。
错误始终指向工作区里的真实 `.v` 文件和行列。保存后的端口或状态布局变化由运行时 V2 在下一帧
事务式替换定义；如果当前已有电路板打开，则保守拒绝并在下次启动重新编译。

**热加载（同形状逻辑替换）**：`Component::hotSwap` 把新编译的程序放进**同一个** `Program` 对象里，
所以宿主持有的回调 `user` 指针一直有效，所有实例从下一个周期起就跑新逻辑——不用重启、不用重新注册、
生成代码也不用变。前提是形状不动：端口（名字、方向、位宽、顺序）与 `state_words` 必须一致；
编辑器里"保存源码"先走 `Project::saveAndReloadSource`：同形状时直接替换执行计划；端口或状态布局
变化时照常原子保存，再在下一帧调用 `Project::reloadLive` 替换完整定义。替换只在没有电路板时开放，
避免移动正在运行实例的布局。

**运行中类型生命周期**：`tc.component.runtime` V2（`sdk/tc_component_runtime.h`）是加载期注册闸门
（`tc.component.types` 只在 `tc_mod_load` 期间开放）的窄例外。`register_type` 新增类型，事务式
`replace_type` 更新同一 `custom_id` 的名字、端口、状态布局和程序，`unregister_type` 删除调用 Mod
拥有的类型；三者都只在元件模型空闲时开放（游戏线程、没有电路板＝主菜单，正是 Mod 自己的编辑页
所在的位置）。`Project::registerLive` / `reloadLive` / `unregisterLive` 是对应封装；加载器太旧时返回
`TC_COMPONENT_TYPES_ERR_UNAVAILABLE`，调用方保留“下次启动生效”行为。编辑器在**下一帧**
（`on_frame`，不在页面绘制里）提交变更。

完整替换调用游戏原生 `update_custom_prototype`，不会先删除类型再用同一个 ID 重新导入；后者会被
游戏内部另一张 ID 索引当成重复。每次成功注册、替换或删除都会推进菜单 generation。游戏下次绘制元件栏时，加载器先调用游戏自己的
菜单重建函数，把当前类型表重新复制到可见栏，再继续原始绘制；因此新建、复制、改名、端口布局变化
和删除都会同步到“自定义”分类，不需要补写工坊文件，也不需要重启进程。

文件管理器 API（`createSource` / `duplicateSource` / `renameSource` / `removeSource`）让编辑器
能真正增删项目文件：每个操作先校验名字与源码（新建、复制还要编译通过），再原子改写清单，失败
时磁盘上什么都不变。新建与复制会分配新的 `custom_id`，并把 `type_id` 写成
`<setTypePrefix 的前缀>/<相对路径去掉 .v>`；改名只移动文件、改清单里的 `source`，保留元件身份，
因此电路里已经用到的元件不会失效；每个项目至少保留一个元件，删除最后一个会被拒绝。插件通常用
`setTypePrefix(host->mod_id)` 让新元件的 type id 落在自己的名字空间里。

`Project::createNew(directory, ProjectCreateOptions, diagnostics)` 创建一个最小但完整的 format 2
项目：先校验项目 ID、源码根、组件 key 和顶层模块，再编译 `initial_source`；成功后写入默认
`.v`，最后原子提交 `tc-verilog.json`。目标清单或默认源码已经存在时绝不覆盖；失败时当前
`Project` 对象保持不变。游戏内“新建项目”页使用的就是这条 API，而不是另写一套清单生成逻辑。

## 游戏内源码编辑页

`example.tc-verilog-ram.mod` 现在同时申请 `services` 与 `ui_page`：从主菜单打开 **Verilog 编辑器**，
左侧选择项目中的 `.v` 文件，右侧是 256 KiB 多行编辑区。文件名后的“已修改”表示未保存；
“检查语法”只编译，“保存源码”执行“编译通过 → 原子保存”，“撤销改动”回到最近一次保存内容。
诊断在页内显示阶段、完整文件、元件 ID、行和列。保存成功后，同形状逻辑立即换入；端口或 RAM
状态布局变化则排入下一帧的完整定义替换，并同步刷新元件栏。

源码框不是普通文本框，而是 `sdk/tc_code_editor.h` 里的编辑器控件（见 [ui.md](ui.md#代码编辑器控件sdk-)）：
行号装订区（光标所在行高亮）、Verilog 关键字／系统任务／编译指令／数字／字符串／注释着色、
回车按上一行缩进并随 `begin`／`end` 之类进退一级，Tab 插入真正的制表符；光标、选区、拖选、
撤销重做与 IME 仍由引擎自己的 `InputTextMultiline` 负责。打开页面时焦点直接落在源码框上，
`Ctrl+S` 等同于“保存源码”（同样先编译验证）。切换文件时控件 id 带文件序号，所以每个文件有
自己的编辑会话与撤销栈。

左侧文件列表上方是文件管理器：**新建 / 复制 / 改名 / 删除**。新建与复制写入一个可编译的模板
（缓冲器）并立即登记为新元件，名字框里可以写相对目录；改名和删除都先给出确认或原名字，删除会
同时移除清单条目与 `.v`。任何文件有未保存改动时这四个按钮会被禁用，先保存或撤销才能继续，
避免把编辑中的内容冲掉。新建/复制在下一帧注册，改名和端口/状态布局变化在下一帧替换，删除在
下一帧注销；成功后运行模型与可见元件栏一起刷新。加载器不支持 V2，或当时已有电路板打开时，
才回退成“重启游戏后生效”。同形状逻辑保存仍直接热换执行计划，从下一个周期起生效。

删除之后再新建**同名**文件也走同一条注册路径：编辑器给新文件的 `custom_id` 由文件名算出，
所以它会与刚注销的那个 id 相同；加载器记得自己注销过哪些 id，再次注册时改走游戏原生的
`update_custom_prototype`，并在成功后把 id 补回元件栏读取的有序 live 列表（游戏的删除只从
这张列表里摘掉 id，原型表里那条仍在，直接 `add_custom_prototype` 会把同一个 id 判成重复）。

页面按真实剩余内容区自适应：宽屏为文件／源码左右分栏，窄屏自动改成上下布局；宿主页头为单行
“返回 + 中文页名”，不会再与插件正文重叠。真机回归会自动打开该页并从 OpenGL 帧缓冲保存截图，
用于检查中文、按钮宽度、分栏、源码区和编译输出区是否被裁切。

真机回归 `tests/tc-verilog-project-playtest.ps1` 在隔离游戏副本中运行两次：第一次断言包内项目
注册为 5 输入/1 输出 V3 RAM 并生成工作区，并在抓到的整页截图里按编辑器调色板数像素（关键字
至少 500 个、数字至少 60 个纯色像素），所以“截图里真的有高亮”是自动判定的，不靠肉眼；第二次
把工作区源码故意改坏，断言启动不会覆盖它，并从日志核对 `.v:2:1`、元件 type ID 与编译原因。

## 最小 RAM

```verilog
module ram16x8(
    input wire clk,
    input wire we,
    input wire re,
    input wire [3:0] addr,
    input wire [7:0] wdata,
    output wire [7:0] rdata
);
reg [7:0] mem [0:15];

always @(posedge clk) begin
    if (we) mem[addr] <= wdata;
end

assign rdata = re ? mem[addr] : 8'bz;
endmodule
```

## 接到 TCLogicIOV3

插件把 `tc_verilog.cpp` 一起编译，并让 `Component` 活到 Mod 卸载之后。端口名、顺序和位宽会
直接生成 V3 引脚表，寄存器/RAM 布局会生成 `state_words`：

```cpp
#include "tc_component_types.h"
#include "tc_verilog.hpp"

static tc::verilog::Component ram;

int register_ram(const TCHost* host, const std::string& source) {
    std::vector<tc::verilog::Diagnostic> errors;
    tc::verilog::ComponentMetadata metadata;
    metadata.custom_id = 0x5443564552414d31ULL;
    metadata.type_id = "my-mod/ram16x8";
    metadata.name = "TC-Verilog RAM"; // 留空则采用 module 名
    metadata.description = "16 x 8 synchronous-write RAM";
    metadata.gate_cost = 16;
    metadata.delay = 1;
    metadata.pin_lane = 3.0f;

    if (!ram.compile(source, metadata, &errors)) {
        // errors[0].line / column / message 可直接显示到编辑器。
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    }
    return ram.registerWith(host);
}
```

`Component` 拥有程序、元数据字符串和 pin 数组，不能复制或移动；通常把它声明成插件静态对象。
注册成功后定义冻结，避免改变端口或状态布局而让已经生成的游戏元件失效。注册前的 `compile` 是
事务式的：新源码有错误时，上一份有效程序和定义保持不变。执行回调不抛异常；若 IO 数量、
状态容量或程序指针不匹配，输出全部为 Z。需要自己管理定义时仍可直接使用 `Program::callback`。

这一版已经包含磁盘清单、`.v` 源码、不覆盖玩家修改的持久工作区和游戏内源码编辑页，但尚未把
编译器作为宿主服务暴露。`Component` 已能生成游戏默认元件外观（端口和引脚），
但自定义图形与碰撞箱仍需走 render/geometry 服务；它们会在后续外观编辑器切片里合并。
