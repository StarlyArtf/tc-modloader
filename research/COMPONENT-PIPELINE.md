# 元件链路研究：2.1.334 指定构建

日期：2026-09-16。第一阶段证据来自静态反汇编；第二阶段已在独立安装和
USERPROFILE 中运行导入探针，见文末。静态链路确认不等于完整放置／仿真验证。

EXE SHA-256：`8875da0e88cc878cb0fce30cfb63d20c5500cae341608af4d786649b88c8eb21`。
下列地址均为该文件首选映像基址 `0x140000000` 下的 VA，运行时需要 ASLR 修正。

## 主要结论

1. 自定义元件 ID 是 **64 位**。原 SDK 的 `TCPrototypeKind::custom_id`
   被误写为 `uint16_t`，现已修正。哈希表起始槽位使用低 16 位，随后比较完整 ID；
   两者不能混为一谈。原 `getPrototype(Custom, id, out)` 直接调用独立 getter，
   已经传入 64 位 ID，所以不是所有旧调用都会出错；错误主要在公开的 key 布局。
2. 游戏的自定义原型入口从 `circuit.data` 解析电路，然后构造原型、更新设计及依赖。
   往 `custom_prototypes_set` 写一个复制的内置模板，不会自动完成这条链路。
3. 仿真有明确的预处理、代码生成、动态编译和 JIT 执行阶段。
   当前没有发现可以直接向原型对象填入“每周期 C++ 回调”就完成元件行为注册的证据。
4. 自定义电路封装与新增原生逻辑种类是两条不同路线：前者可以继续研究游戏现有
   电路导入／展开机制；后者还必须研究 kind 分派、信号存储及状态更新。

## 1. 注册链路和 ABI

已核对的直接调用关系（省略错误处理和析构）：

```text
update_custom_prototype (0x140178270)
  拼接 circuit.data → file_get_bytes
  → add_custom_prototype (0x1401780c0)
      → parse_state
      → reload_custom_prototype (0x140177080)
          → custom_prototypes_set (0x140126790)
          → update_custom_design (0x140175db0)
          → update_uses (0x140176e10)
      → update_uses
```

`update_custom_prototype` 在 `0x14017831c–0x14017832d` 拼接 `circuit.data`，
在 `0x14017841d` 读取内容，在 `0x140178463` 调用 add。

`add_custom_prototype` 的机器级参数分布已明确：

| 位置 | 已确认的机器级行为 | 语义状态 |
|---|---|---|
| RCX | 保存到 R12；返回前写入 16 字节；RAX 返回同一地址 | 隐藏结果指针 |
| RDX | 读取 16 字节，经 reload 进入原型 `+0x10` 字符串 | 名称参数，静态数据流支持 |
| R8 | 读取两个 QWORD，交给 `parse_state` | 电路字节串 |
| R9 | 读取 16 字节，经 reload 传递 | 来源目录／路径，调用者数据流支持 |
| 返回对象 +0 | 初始为 0；成功路径存入 reload 的低字节 | 成功标志候选 |
| 返回对象 +8 | 初始为 0；成功路径存入解析结果 `+0x50` | 自定义 ID，后续作为注册键 |

这里不是此前推测的“add 本身还缺几个栈参数”：add 没有读取调用者的栈参数。
复杂栈参数属于它内部调用的 **reload**。reload 在 8 次 push 和 `sub rsp,0xe68`
后，从 `rsp+0xed0/0xed8/0xee0/0xee8` 读取第 5–8 个机器级参数。
add 明确准备了这四个槽位。

第一阶段未发布可调用封装。第二阶段在验证临时字符串生命周期、同 ID 更新和
快照释放后提供了实验 API；失败后的部分更新和复杂依赖刷新仍未验收。

## 2. ID 和原型内嵌数据

`get_prototype`：

- `0x1401260d1` 比较 kind 是否为 `0x4e`。
- `0x140126110` 执行 `mov rcx,QWORD PTR [r8+0x188]`。
- `0x140126123` 跳转到 `get_custom_prototype`。

`get_custom_prototype`：

- `0x140125fec` 用 `movzx edx,si` 提取初始槽位，槽跨度 `0x5b0`。
- `0x140126029` 用 `cmp rsi,rax` 比较完整 64 位 ID。
- 冲突时槽位递增并按 16 位回绕，继续比较完整 ID。

因此 lookup key 至少覆盖 `0x190` 字节，而不是只包含 16 位 ID。

reload 在 `0x1401771e2` 深复制解析出的电路数据，随后将该 0x48 字节对象的字段
写入局部 Prototype 的 `+0xe0..+0x127`。证据：局部原型基址为 `rsp+0x880`，
写入区域为 `rsp+0x960..+0x9a0`。它还复制另一个 0x400 字节块到原型 `+0x140`。
这些区域说明原型不只是名称、引脚和图标。尚不把其全部字段命名为稳定 SDK 类型。

## 3. 放置与仿真链路

`board_add_component` (`0x14013f560`) 对 custom kind 分支调用
`get_custom_prototype`（调用点 `0x140140116`）。这确认放置过程依赖自定义原型表，
但不能推出原型表是放置的唯一前置条件；菜单来源、放置参数和保存链路仍待验证。

```text
process_request (0x14025edc0)
  → preorder (0x140180d00)
      → get_custom_prototype
      → infer_size / connect / set_circular_dependency / set_critical_path
  → generate_source (0x140251e10)
      → add_circuit_code (0x140226d50)
  → handle_request_compile_and_run (0x14021cf20)
      → compile.dll!compile（动态解析的间接调用）
      → createThread → jit_function → jit
```

上图是函数间关系，不表示每种请求都顺序执行每一步。process_request 有多条分支。

动态导入关系也已交叉核对：初始化函数在 `0x14021d40b` 加载库，
`0x14021d426` 解析函数，结果写入 `Dl_3590324229_`；compile-and-run 在
`0x14021d004` 调用同一槽位。只读数据中的名称分别是 `compile.dll` 和 `compile`。
本机 compile.dll 的该导出继续调用 `compile_source`，后者调用 front_end 和 middle。
没有据此把生成语言猜成 C、Nim 或 LLVM IR。

`jit` 在 `0x14021cce9` 复制代码字节，`0x14021ccf3` 计算“映射基址 + 入口偏移”，
并在 `0x14021ccfe` 跳转执行。该证据足以确认机器码执行路径。

`add_circuit_code` 在 `0x140227284–0x14022729d` 按一字节 kind 查跳转表
`0x140534a60`。表中 `0x4e` 指向共享分支 `0x140227e30`，不是已发现的插件回调槽。
这不证明游戏中绝对没有其他扩展入口，只说明不能靠修改 Prototype 外观字段
给这个分派增加新语义。

## 4. 重复验证

在源码目录运行（无需启动游戏）：

```powershell
node tools/Inspect-Components.js 'D:\p\Turing Complete.exe'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
& C:\msys64\ucrt64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -static -Wno-cast-function-type tests/game-model.cpp -o build/game-model-test.exe
& .\build\game-model-test.exe
```

本轮结果：26 项真实 EXE 静态检查通过；SDK 模型回归通过，测试 ID 改为
`0x123456789abc007b`，覆盖高 48 位，另检查 key `+0x188` 的 8 字节读回。
模型测试仍使用替身函数，不能作为游戏元件注册成功的证据。

检查脚本严格拒绝其他哈希的 EXE。反汇编及 JSON 报告输出到忽略的
`build/component-research/`；仓库只保留分析方法和研究结论，不提交游戏二进制。
编译请在源码目录执行：本机游戏根目录的运行库会干扰 MinGW 子进程启动。

## 5. 后续完整元件验收目标

优先走现有“电路封装元件”路径：

1. 在独立测试安装与 USERPROFILE 中验证导入参数、结果及错误状态（第二阶段已完成
   直接调用探针，未运行被动 Hook，见下节）。
2. 从已有可运行的小电路构造测试副本，使用新的 64 位 ID，通过正常导入路径生成原型。
3. 验证菜单出现、放置、连线、保存、退出重载，以及组合逻辑真值表和有状态电路行为。
4. 这些完成后才能把实验接口升级为完整元件 API；新增原生 kind／代码生成分支另立实验，不能用以上
   电路封装测试冒充“任意 C++ 元件行为已支持”。

## 6. 第二阶段：导入封装与真实游戏验证

新增 `TCComponentModel`：`importCircuit`、`updateFromDirectory`、`releasePrototype`
和 `readiness`。显式隐藏返回参数已在真实调用中验证，返回的非零 ID 为
`5751619089986870539`，并能从自定义原型表查到对应对象。

新增发现：`rawNewString` 只分配容量，不设置字符串逻辑长度。其 `rsp+0x20` 始终
保持 0，返回对象包含这个 0 和 payload 指针。旧 SDK 的字符串 setter 未设置长度，
原测试替身却自动设置长度，掩盖了错误。现已修复生产代码和替身，并在实机用
`setPrototypeName` 验证实际长度为 12 后释放快照。

输入不能使用随函数退出销毁、但标为 bit 62 静态的插件字符串：
`nimAsgnStrV2` 在 `0x1400183b7` 检测这个标志，`0x140018447` 可直接共享 payload。
封装改用游戏 `rawNewString` 分配普通 payload、显式设置长度，结束后调用
`deallocShared`。二者均访问 `allocator__system_u7164`，要求在同一线程释放。

实机探针步骤：

1. 全新复制游戏文件，设置新的 USERPROFILE 和 APPDATA，仅加载开发探针。
2. 从先前隔离测试目录复制 162 字节 NAND 关卡电路作为 fixture，原文件前后 SHA-256
   一致：`1a314033854c7b15d1bbd482df4f25c047dc8ea248aca85add3f12f064cabc90`。
3. 从字节导入 `Probe component`，检查返回 ID、表中存在、名称及长度。
4. 释放 getter 返回的原型，再从目录更新为 `Renamed component`；ID 和原型总数不变，
   新名称和长度正确，再次释放。
5. 指向不存在的子目录，返回 Rejected，Nim 错误状态仍为 0。
6. 关闭本测试启动的游戏进程。未改变玩家安装的启用状态或存档。

结果：两次隔离导入测试通过；最后一次额外覆盖了真实分配器下的字符串 setter 修复。
最终证据位于本机忽略目录
`build/component-playtest-7e08cf840d8442f996e24af7c4b35b76/game/tc-modloader-data/loader.log`。
29 项 EXE 静态检查通过，模型回归和新组件单元测试通过。

复现：先在源码目录编译 `tests/component-probe.cpp` 为
`build/component-probe.dll`（`g++ -std=c++17 -O2 -static -shared`），再执行
`tests/components-playtest.ps1 -Circuit <已有电路的完整路径>`。
脚本复制 fixture 和游戏到新的 build 子目录，探针不随玩家包发布。

边界：fixture 原型输入和输出数均为 0。这是解析／注册／更新／释放验证，
不是可用自定义端口、菜单注册、持久化或仿真行为验证。没有伪造高层成功结论，
也没有试图在导入失败时清除 Nim 错误标志。模拟器停止状态由调用方保证，
封装只强制绑定线程一致，不会主动暂停或重置玩家电路。

## 7. 第三阶段：电路格式与最小 AND 元件（步骤 1）

`circuit.data` 的首字节是序列化版本，其余字节是原始 Snappy 块（不是 zlib，也
不是 JSON）。`parse_state` 按 `data[8]` 对应的版本跳转；本构建支持 v0–v16，
现有自定义元件使用 v13/v14。v13/v14 的顶层结构一致：

```text
i64 seed              // 同时作为后续自定义元件 ID
u32, i64, i64, bool, u64
i16 头部序列长度 + i64 元素...
u16 长度 + UTF-8 字符串
u8, u16
u16 长度 + 字节...
u16 长度 + UTF-8 字符串
512 字节线色面板（seed 非零时）
i64 组件数 + 组件...
i64 连线数 + 连线...
```

v13/v14 的组件公共字段基本一致，差异在于 v14 在组件 `bits` 后多了两个 i64
字段，然后才读第二个 bool 和 `init_data`。已确认的组件 kind：

| kind | 含义 | 关键字段 |
|---|---|---|
| `0x4f` | 自定义输入引脚 | `bits` 映射为引脚 raw word size；名字来自 `name` |
| `0x51` | 自定义输出引脚 | 同上；`value_i16` 区分输出顺序 |
| `0x04` | 两输入一输出 AND | 输入在 `(x-1,y±1)`，输出在 `(x+2,y)` |
| `0x4e` | 自定义元件实例 | `custom_data` 指向被引用元件的 64 位 ID |

线段的 16 位编码是 `(方向 << 13) | 长度`，低 13 位为 0 表示路径结束。该构建使用
8 个方向；按 `0=东、1=东南、2=南、3=西南、4=西、5=西北、6=北、7=东北`
解释时，已有多组引脚端点和现有电路连通性一致。组件坐标是原理图网格坐标，
不是渲染像素。

新增 `tools/circuit_format.py` 作为只读／可写研究工具。它依赖
`python -m pip install python-snappy`，只用于开发和 fixture 生成，不是玩家运行时
依赖。测试路径不依赖 Python：`tests/and-component-fixture.cpp` 自带只写 literal
块的 Snappy 生成器，`tests/and-component-probe.cpp` 在隔离游戏中验证导入结果。

步骤 1 的固定 fixture 是两输入一输出 AND，ID `0x414E44325F303031`。实机导入返回同一 ID，
原型名称为 `AND2 Test`，输入 2、输出 1，三个引脚的 raw word size 均为 1。该证据覆盖
“电路封装元件的引脚、位宽和内部电路已进入游戏原型表”，但不等于菜单、放置、连线、
保存重载或逻辑真值表已经验收。

## 8. 第三阶段：菜单放置模型（步骤 2）

组件菜单最终调用 `add_component__presenterZutilitiesZhelper95functions_u5918`。
它的第一个参数不是 board，而是 presenter/board 容器；board 位于 `context+0x78`。
`handle_update_wire` 的第一个参数就是该容器指针，因此可以在开发探针中捕获。

board 布局（已通过放置前后计数与现有 wire 代码交叉验证）：

```text
board = context + 0x78
board + 0x00 / +0x08   组件序列长度 / payload
board + 0x20 / +0x28   导线序列长度 / payload
```

序列 payload 有 8 字节头，元素从 `payload + 8` 开始；board 组件步长是 `0x238`。
这和保存格式的组件对象大小一致。菜单放置请求只用以下字段：

```text
+0x00  u8   kind（自定义为 0x4e）
+0x02  i32  位置，低 16 位 x、高 16 位 y
+0x06  u8   旋转/方向
+0x188 i64  自定义元件 ID
```

其余字段沿用菜单模板中的默认值，然后由 `add_component` 查找原型并调用
`board_add_component` / `board_commit_add`。新增 `tests/component-placement-probe.cpp`
在隔离 sandbox 中执行这条路径：注入的 AND 元件以 kind `0x4e`、位置 `(30,0)`、ID
`0x414E44325F303031` 出现在 board 组件数组中。该测试证明菜单的模型路径可以放置插件
注册的电路元件；鼠标命中测试和列表视觉渲染仍属于后续 UI 自动化范围。

## 9. 第三阶段：引脚偏移与真值表（步骤 3）

`get_position` 把组件位置与引脚在原型描述符中的偏移相加。实际引脚描述符从
`prototype + 0x68` 或 `+ 0x88` 指向的 payload 的 `+8` 处开始，每个描述符 `0x38` 字节，
偏移点位于描述符 `+2`。因此公开 helper 返回的 `TCPin*` 需要再加 8 字节才是真实描述符；
这是本次新增验证暴露出的一个 SDK 封装边界。

指定构建上已核对的引脚偏移：

| kind | 类型 | 输入偏移 | 输出偏移 |
|---|---|---|---|
| `0x4f` | Input Pin | `(0,0)`（保留/内部） | `(3,0)` |
| `0x51` | Output Pin | `(-3,0)` | `(0,0)`（保留/内部） |
| `0x04` | AND | `(-1,-1)`, `(-1,1)` | `(2,0)` |
| `0x3f` | 2-bit input | 无 | `(0,-1)`, `(0,1)` |
| `0x44` | 1-bit output | `(-1,0)` | 无 |
| `0x6d` | 2-bit splitter | `(-1,0)` | `(1,-1)`, `(1,0)` |

导线的 16 位段编码仍为 `(方向 << 13) | 长度`。方向表是
`0=东、1=东南、2=南、3=西南、4=西、5=西北、6=北、7=东北`。
新增的 `tests/and-component-netlist.js` 按该表和上述引脚偏移解析 fixture，得到
`00,01,10,11 -> 0,0,0,1`。这一步同时发现 fixture 初稿把“南”和“北”写成了
`0x2000`／`0x6000`，正确值是 `0x4000`／`0xC000`；修正后游戏导入、sandbox 放置和
网表真值表测试全部通过。

当前真值表验证的是序列化网表本身。真实游戏的关卡测试循环、每个周期输出读取和
有状态时序验证仍待下一步完成。

## 10. 第三阶段：保存路径与重载（步骤 4）

保存目录中的 `levels.txt` 用

```text
"<level>",<bool>,"<schematic name>",
```

记录每个关卡当前选择的 schematic。电路文件位于
`schematics/<level>/<schematic name>/circuit.data`，例如
`schematics/and_gate/Default/circuit.data`。只把测试电路写到
`schematics/<level>/circuit.data` 不会被当前关卡选择。

fixture 生成器现在同时生成关卡电路 `and2_solution.data`：输入 `0x3f`、自定义实例
`0x4e`、输出 `0x44`，三段导线分别连接两个输入引脚到自定义元件并连接输出。
引脚偏移来自第 9 节，北向和南向导线使用修正后的 `0xC000` 和 `0x4000` 高位。

`tests/component-persistence-playtest.ps1` 在同一个隔离 USERPROFILE 中连续启动两次：
两次都由插件重新导入电路定义，并重新加载 and_gate 的保存文件。探针在 board 组件
序列中找到 kind `0x4e`、ID `0x414E44325F303031`，并确认导线数为 3。保存文件
SHA-256 在两次启动前后保持一致。

边界：`save_level_data`、`save_all_design_changes` 和 `save_level_design` 的直接调用
可以返回而不改写磁盘文件，说明它们仍依赖 UI 状态或脏标记。当前已验证的是“已保存电路
重启后正确重载”；运行时修改经游戏 UI 保存落盘仍需继续研究。

## 11. 延迟统计链路（进行中）

用户实测：自定义 AND2 自身延迟为 2，但放进关卡／沙盒／元件工坊后，统计出的总延迟
不包含它。本节记录已确认的链路和已排除的假设。

新增只读工具 `tools/xref.js`：直接读 PE 节表 + COFF 符号表，扫描 `.text` 里的
`E8/E9` 直接调用与常见 rip 相对内存引用，回答“谁调用了 X”“谁引用了全局表 X”。
注意 objdump `-t` 打印的符号值是**节内偏移**，必须加上所在节的 VMA 才是真实 VA；
反汇编注释里的地址才是可直接对比的 VA。

已确认的代价函数分派（VA 为首选基址 `0x140000000`）：

```text
get_cost__modelZscores_u2321          0x140158d60   (component) -> (gates, delay)
  ├─ get_gate_cost__modelZscores_u2304.part.0  0x140158630
  └─ get_delay_cost__modelZscores_u2316        0x140158b40
       ├─ kind 0x4f / 0x51 -> [component+0x130]
       ├─ kind 0x4e        -> get_custom_prototype(id) 后读 [prototype+0x138]
       └─ 其余 kind        -> get_delay_cost__modelZscores_u2270(按 size 的 log2 公式)
```

也就是说，**自定义元件实例的代价只能来自原型字段** `prototype+0x130`（门数）和
`prototype+0x138`（延迟）。真实导入的测试元件这两个字段确实是 1 和 2（插件日志
`inserted custom cost gate=1 delay=2` 与探针 `prototype gates=1 delay=2` 两次独立确认）。

`add_cost__modelZscores_u2110(kind, pair)` 不是“写入某 kind 的门数/延迟”：
它对不被 `0x140526840` 位图排除的 kind 调用 `insert_cost__modelZscores_u49`，
在 `component_cost_buffer` 里插入一条表项，并把 `component_costs[k]`（k > kind）
的起始下标整体右移。而 `get_delay_cost` 的 0x4e 分支只读原型字段，
**所以这条插入对自定义实例的延迟没有影响**。这解释了此前“插入成功但总延迟不变”。

总延迟的算法：`preorder__modelZsimulationZpreorder_u8749`（0x140180d00）在
`0x140184905` 对每个元件调用 `get_cost`，把“入线延迟最大值 + 本元件延迟”写回
端口项 `+0x38`，并在 `+0x20` 里维护最大值（0x1401848dc / 0x1401849ac）。
这条链路上的延迟增量就是 `get_cost(...).delay`，对自定义元件即 `prototype+0x138`。

界面上的分数不是实时求和：它来自编译/预排序结果。调用链是
`main -> preorder_from_frontend__presenterZutilities_u17092`（0x140316750）
→ 编译线程 → `await_build_result__modelZutilities_u6541`（0x1402ce2a0）
→ `try_update_preorder_result__modelZsimulationZcompile95thread_u3045`（0x140262370），
结果经响应通道回填。`build_scores__presenterZboard95uiZmenu95bar_u886`（0x14045b4f0）
只负责显示：门数取 `[score+0]`、延迟取 `[score+8]`、`[score+0x20]` 是字节标志。

旁证（已执行）：`tests/component-cost-playtest.ps1` 在隔离沙箱里直接调用
`load_level` 装载关卡并跑 8 个周期，三种模式（内置 AND／自定义 plain／自定义 insert）
的 `build_scores` 全部读到 `gates=0 delay=0`，连**内置 AND** 也是 0。
说明脱离 UI 的 `load_level` 路径不会产生 build result，因此不能用来判定
“总延迟是否计入自定义元件”。后续判定必须在真实 UI 流程里做。

为此新增诊断 Mod `dist/dev.cost-watch.mod`（源码 `tests/cost-watch.cpp`，
只挂钩子、不修改状态，日志前缀 `cost-watch:`）。它同时观测 `get_cost`、
`get_delay_cost_u2316`、`get_gate_cost_u2560`、`preorder_u8749` 与 `build_scores`，
并周期性打印界面正在显示的 `gates/delay`、各 kind 的代价返回值以及原型字段。

### 11.1 早期结论：定义头缓存与展开电路的差异（未解决声明延迟覆盖）

真机日志（两次会话）给出的关键事实：

| 板子 | 界面分数 |
|---|---|
| 输入 → **内置与门(0x04)** → 输出（`get_cost` 报 `g=1,d=1`） | gates=1 **delay=1** |
| 同一板子换成 **自定义 AND2(0x4e)**（`get_cost` 报 `g=1,d=2`） | gates=1 **delay=1** |

也就是：自定义实例的 `get_cost` 确实返回 2，但整板延迟仍是 1 —— 板级延迟走的是
`preorder` 对电路展开后的真实关键路径（这里就是那个与门的 1），并不读原型的缓存延迟。
门数同理来自真实电路。

这里涉及**定义头部的两个 i64**。它们不是随便的填充值，而是**该设计自身的
缓存统计 `(门数, 延迟)`**。证据一：用户自己元件工坊里的设计文件
（`%APPDATA%\Turing Complete\schematics\foundry\*`，本构建写 v16）分别是
`4or=(3,2)`、`8or=(7,3)`、`1and8=(8,1)`、`half-add=(4,2)`、`RS=(12,5)`、
`RV32I/ALU=(8302,109)`，零扩展类设计为 `(0,0)`，数值都与"门数 / 关键路径深度"吻合。

证据二（受控实验）：同一个 fixture 只改头部这两个 i64，重新导入后读取原型字段：

| 头部写入 `(a, b)` | 注册后的 `prototype+0x130 / +0x138` |
|---|---|
| `(3, 2)` | `(1, 2)` |
| `(0, 0)` | `(1, 0)` |
| `(7, 5)` | `(1, 5)` |

即解析时游戏**重算门数**，但**原样保留头部的延迟**。因此头部写成多少，元件信息/代价
面板就报多少。

最初的 fixture 把头部写成 `(3, 2)`，那其实是从 `foundry/4or` 抄来的数值，于是出现了
"元件自称延迟 2、整板却按 1 计"的现象。修复：

* `tests/and-component-fixture.cpp` 与 `examples/circuit-and/and_fixture.hpp` 现在写
  `(1, 1)`（单个与门的真实门数与关键路径），并保留命令行覆盖以便继续做对照实验。
* `TCGameModel::setPrototypeGateCost` / `setPrototypeDelay`（以及
  `TCPrototypeBuilder::setDesignCost`）允许插件在注册前把这两个字段改写成设计自己的
  统计值；示例 Mod 现在会显式写入并打印它们。
* 示例 Mod 不再调用 `addComponentCost(0x4e, ...)`：自定义实例的代价只读原型字段，
  该调用对实例延迟没有影响（保留 API 供其它 kind 使用）。

沙箱复验：导入修复后的定义，探针读到 `prototype gates=1 delay=1`，与真机整板
`delay=1` 一致；内置与门对照同样为 1。

### 11.2 当前安装的调试覆盖排查（2026-09-16）

再次检查安装目录发现 `tc-modloader-data/plugin-data/example.circuit-and/design-stats.txt`
仍为 `1 5`。最新已有会话日志同时显示原型 `gates=1 delay=5`、板级
`displayed gates=1 delay=1`，且板上存在 AND2 自定义实例。这与上一节的内部电路
展开统计一致，不能据此认定该实例完全没有计入延迟；该样例门数已计入。

用户确认 `1 5` 是刻意设置的对照实验，用于检查标称延迟是否进入总延迟。
此前误将该文件停用，现已恢复为 `design-stats.txt`，保留 `(1,5)` 继续测试。
没有改动内部电路，也没有强行改写板级分数。现有日志确认标称值 5 未直接成为
整板延迟；不能将恢复默认值或让两个显示值一致当作统计问题已修复的证据。
当时 SDK 仅将 `setDesignCost` / `setPrototypeDelay` 作为缓存元数据设置，
不改变内部电路或整板关键路径；后续实现见 11.3。

验收边界：上述板级数据来自已有日志，并非本次重新执行三种模式的 UI 测试。
关卡、沙盒、元件工坊仍需分别验证串联、并联和嵌套元件；不能将单个 AND2 的
结果外推为所有 Mod 元件均已正确统计。

### 11.3 声明延迟纳入编译关键路径（本次实现）

11.1 的缓存修正只消除了示例显示差异，未实现用户需要的“声明为 5，总延迟也计 5”。
现在加载器在原生 Mod 成功初始化时记录所注册的自定义 ID；在固定构建的
`preorder` 延迟计算调用点（RVA `0x184905`）接入声明延迟处理。
原游戏 EXE 文件不改写，安装时检查调用点原始字节，进程内跳板只作用于该计时调用。

从编译图的节点、父实例 ID、输入/输出网络建立计时依赖，将已登记 Mod 实例及其内部
节点视为一个计时节点，使用原型 `+0x138` 的声明延迟求最长路径，再向原编译器的端口
到达时间传播返回增量。内部逻辑仍按原电路执行；没有直接改 UI 数字，没有把 Mod
延迟简单加到全局分数上；门数沿用游戏原来的递归计算。

嵌套使用最外层已登记实例的声明，避免重复计费。含反馈、多驱动、收缩后成环或数值
溢出的图回退到原生统计；反馈/多驱动等求解失败会输出诊断。运行时才新增的原型暂不
在初始化登记范围内，不能将本次修复描述为任意有状态 Mod 的完整计时支持。

诊断修正：`preorder_u8749` 实际有 8 个机器级参数，旧观察 Hook 仅转发 3 个；现已
修正，并允许观察插件在核心加载器占有该 Hook 时继续观测其他接口。门数函数第二
参数是“跳过自定义元件”，计完整门数必须传 0。测试里的内置与门连线也按原生引脚
位置修正。观察统计增加锁，避免编译线程与 UI 线程并发访问容器。

`tests/component-timing-playtest.ps1` 使用独立存档调用游戏异步编译流程，等待界面消费
结果，再通过同步编译交叉验证；对实际 `build_scores` 的显示参数和编译统计都做断言：

| 用例 | 门数 | 总延迟 |
|---|---:|---:|
| 关卡：单个 Mod，声明 5 | 1 | 5 |
| 关卡：两个串联，声明各 5 | 2 | 10 |
| 关卡：两个并联，声明各 5 | 2 | 5 |
| 关卡：声明 0 | 1 | 0 |
| 原生与门对照 | 1 | 1 |
| 嵌套 Mod，外层声明 7 | 1 | 7 |
| 沙盒：单个 Mod，声明 5 | 1 | 5 |
| 元件工坊：单个 Mod，声明 5 | 1 | 5 |

报告为 `build/timing-*-report.txt`。图算法另有串并联、嵌套、断开分支、原生节点、
反馈回退、非法索引和溢出回退测试。用户的 `design-stats.txt` 保留 `1 5`。
