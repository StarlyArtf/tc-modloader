> **研究日志（非发布文档）。** 本文按时间顺序记录反推过程、实验设计与证据，其中包含
> 已废弃的路线、失败实验与当时尚不确定的推断。面向使用者的结论请从
> [文档索引](../README.md) 进入，尤其是 [自定义逻辑回调](../sdk/custom-logic.md) 与
> [验证体系](../verification.md)。

# 元件链路研究：2.1.334 指定构建

日期：2026-09-16。第一阶段证据来自静态反汇编；第二阶段已在独立安装和
USERPROFILE 中运行导入探针，见文末。静态链路确认不等于完整放置／仿真验证。

## 目录（按主题定位，正文按时间顺序）

| 主题 | 小节 |
|---|---|
| 注册链路与 ABI、64 位 ID | [1](#1-注册链路和-abi)、[2](#2-id-和原型内嵌数据) |
| 放置、仿真、代码生成与 JIT 链路 | [3](#3-放置与仿真链路)、[12](#12-第-7-步起点代码生成分派表2026-09-16)、[12.1](#121-路线-1-的执行清单已核对符号) |
| `circuit.data` 格式与最小 AND 元件 | [7](#7-第三阶段电路格式与最小-and-元件步骤-1)、[9](#9-第三阶段引脚偏移与真值表步骤-3) |
| 菜单放置与保存重载 | [8](#8-第三阶段菜单放置模型步骤-2)、[10](#10-第三阶段保存路径与重载步骤-4) |
| 代价与声明延迟 | [11](#11-延迟统计链路进行中)、[11.1](#111-早期结论定义头缓存与展开电路的差异未解决声明延迟覆盖)、[11.3](#113-声明延迟纳入编译关键路径本次实现) |
| 内置原型表与有状态元件 | [11.2](#112-内置原型表与有状态元件步骤-5-起点) |
| 原生逻辑回调（真实链路） | [12.3](#123-最小原生自定义逻辑已跑通2026-09-17)、[12.4](#124-原生逻辑-sdk-与示例-mod2026-09-17)、[12.5](#125-代码生成桥接与普通电路共同工作的原生逻辑2026-09-17) |
| 形状与位宽泛化 | [12.6](#126-接口形状泛化2026-09-17)、[12.7](#127-多位宽引脚2026-09-17)、[12.8](#128-位宽形状组合扩展与两个夹具陷阱2026-09-17) |
| 已废弃路线（仅存档） | [12.2](#122-自定义元件内部槽位与首个负结果2026-09-17)、[12.4](#124-原生逻辑-sdk-与示例-mod2026-09-17) 开头标注的整板解释器 |

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

### 11.2 内置原型表与有状态元件（步骤 5 起点）

新增只读探测包 `dev.kind-list.mod`（源码 `tests/kind-list-probe.cpp`，
回归 `tests/kind-list-playtest.ps1`）：枚举 `PROTOTYPES` 表，输出 125 个内置元件的
kind、名称、输入/输出引脚数与引脚偏移。完整表在 `build/kinds.txt`。要点：

- 名称齐全：`0x03 NOT`、`0x04 AND`、`0x0d Delay Line`、`0x0e Register`、
  `0x26/0x27 Counter/Register`、`0x29 Level And Delay Component`、`0x37 Delay Line`、
  `0x77 Auto Delay Line`、`0x76 RAM`、`0x4d Counter` 等。
- 有状态元件（0x0d/0x0e/0x26/0x27/0x37）在原型里**不声明输入引脚**
  （`+0x60 = 0`，`+0x68 = 0`），只有输出。它们的输入位置取自游戏自带的解题电路：
  `campaign/double_buffer/hint_solution.data` 里两个 Delay Line 的连线端点显示
  **输入 `(-3,0)`、输出 `(+3,0)`**（相对元件坐标）。
- 游戏自己的代价函数对 Delay Line 返回 **5 门 / 4 延迟**（真机日志
  `cost[kind=0xd] gates=5 delay=4`），因此以它为基础的有状态封装应声明 `(5,4)`。

新增 fixture 拓扑 `delay`（`tests/and-component-fixture.cpp`）：把 AND2 定义里的
与门换成 Delay Line，接线为 A → `(-3,0)`，`(3,0)` → 输出引脚；关卡侧沿用既有
单件电路。真机结果：导入成功、原型读到 `(5,4)`、编译与界面都是 4 延迟 5 门，
`tests/component-timing-playtest.ps1` 的 `stateful` 用例通过。

尚未验证：跨周期的状态保持、暂停与重置（需要能读取引脚值或以游戏自带关卡测试
作为判定；计划用 `set_sim_test` + 测试状态或 `odd_ticks`/`double_buffer` 关卡做差分验证）。

## 12. 第 7 步起点：代码生成分派表（2026-09-16）

`add_circuit_code` 按 kind 查的跳转表位于 `.rdata` VA `0x140534a60`（256 个
int32 相对偏移）。实测统计：**256 个 kind 映射到 173 个不同处理分支**。

| kind | 含义 | 代码生成分支 |
|---|---|---|
| `0x03` | NOT | `0x14023a653` |
| `0x04` | AND | `0x140239e07` |
| `0x0d` | Delay Line | `0x140238f18` |
| `0x0e` | Register | `0x140239b3a` |
| `0x27` | Register(word) | `0x14022a8d8` |
| `0x3f` | 关卡输入 | `0x140227e30` |
| `0x44` | 关卡输出 | `0x140230df0`（独立分支） |
| `0x4e` | 自定义元件 | `0x140227e30` |
| `0x4f` / `0x51` | 自定义引脚 | `0x140227e30` |
| `0x76` | RAM | `0x140227e30` |

结论：有真实逻辑语义的元件各自**独占**代码生成分支；而**自定义元件、引脚、
RAM 等 36 个 kind 共用同一个分支 `0x140227e30`**（结构型分支）。这解释了为什么
插件无法通过注册表给某个 kind 增加语义：分派表里没有回调槽，自定义元件是靠
"展开内部电路"进入代码生成的。

由此，"治本"的三条候选路线（按可行性排序）：

1. **模拟层替换**：保留游戏代码生成，Hook JIT 入口（`jit`，
   `0x14021cce9`–`0x14021ccfe` 复制代码字节并跳转）或每周期步进函数，在原生
   代码里为插件的元件读写状态、实现自定义语义。与第 5 步共用一个前置条件：
   **引脚 ↔ 仿真状态索引映射**（`simulation_state` 0x9c4000 字节、
   `simulation_output_history_pins`）。
2. **代码生成层替换**：Hook `add_circuit_code`，对我们的 kind 自行发射
   中间代码。需要先弄清它写入的中间语言与 compile.dll 前端的接口（尚未识别）。
3. **分派表改造**：把表中未使用 kind 的条目指向自己的发射函数，效果同 2，
   但更脆弱（依赖表布局与发射 ABI）。

下一步实验：用最小电路（AND + 关卡输入/输出）Hook JIT 入口与步进，dump
`simulation_state` 原始字节与编译后的节点表，配合关卡测试的已知期望值做相关，
得到稳定的引脚↔状态索引映射；拿到映射后第 5 步（状态保持/暂停/重置）与路线 1
可以同时推进。

### 12.1 路线 1 的执行清单（已核对符号）

所需符号在该构建中都存在，可直接 `resolve_symbol`：

| 符号 | VA | 用途 |
|---|---|---|
| `simulation_state__modelZsimulator95types_u81` | `0x1406f1e10` | 仿真状态缓冲区指针；`c_alloc(0x9c4000)`，不是序列对象 |
| `sim_state_read_u64__modelZsimulator95types_u159` | `0x14010f2e0` | 按下标读状态字 |
| `jit_function__modelZsimulationZsimulator95functions_u84` | `0x14021cd40` | 每周期执行的 JIT 包装（hook 点） |
| `sim_do__modelZsimulationZcompile95thread_u3036` | `0x140261850` | 运行/暂停/重置命令入口 |
| `set_sim_test__modelZutilities_u6840` | `0x1402ccea0` | 启动关卡自带测试 |
| `sim_get_test_state__modelZsimulationZcontroller_u26` | `0x1401758b0` | 读测试状态（win/fail） |
| `simulation_output_history_pins__modelZsimulator95types_u85` | `0x1406f1df0` | 输出引脚历史（备选读取路径） |

已执行修正（commit `d750f61`）：旧探针把 `simulation_state` 当 Nim 序列读是
错误的。它实际保存 `c_alloc(0x9c4000)` 返回的裸指针，必须解引用一次，缓冲区
固定为 `0x9c4000` 字节。`load_level` 之后还要调用
`preorder__modelZsimulationZcompile95thread_u3523` 请求异步编译，`sim_do` 才会
真正跑电路。当前 `tests/sim-state-probe.cpp` 同时捕获 model（经 `sim_do`）、
`simulation_state`、`simulation_input_replay` 和
`simulation_output_history_pins`，并用 `build/and2_solution_builtin.data` 做
关卡保存电路。

当前实测映射（四份快照，cycle 序列 `-1 1 2 3`）：

- `simulation_input_replay` 偏移 `0` 与 `8` 都是 2 位输入值 `0,1,2,3`。
- `simulation_output_history_pins` 偏移 `55` 与 `64` 都是输出位 `0,0,0,1`。
- `simulation_state` 本轮只在偏移 `258/259/263` 出现稳定变化，序列为
  `0,0,1,1`，对应 2 位输入的高 bit；低 bit 与输出位不在这个缓冲区里。

因此关卡级输入/输出不要只盯 `simulation_state`；输入在 `simulation_input_replay`，
输出在 `simulation_output_history_pins`。路线 1 要读写插件元件内部网表时，
还需再用自定义元件 fixture 跑一遍，才能确认内部 `0x4f/0x51` 引脚对应的
`simulation_state` 槽位。

### 12.2 自定义元件内部槽位与首个负结果（2026-09-17）

`tests/sim-state-probe.cpp` 现在支持 `TC_SIM_STATE_CUSTOM=1`：它会导入
`and2_component.data`，并加载带 `0x4e` 自定义实例的 `and2_solution.data`。
真机沙箱中，四份快照（cycle `-1 1 2 3`）显示 AND2 自定义实例的内部状态：

| 含义 | 典型 `simulation_state` 字节偏移 | 序列 |
|---|---|---|
| 输入低 bit | `256/257/262/263/266` | `0,1,0,1` |
| 输入高 bit | `258/259/260/261/267` | `0,0,1,1` |
| 输出 | `264/265` | `0,0,0,1` |

游戏 UI 读输出时走 `sim_state_read_u64(264)`；`get_sim_state` 的返回结果
`last0` 也在 0/1 之间变化。已尝试在这两个只读接口上把 AND 输出改成 OR
（仅实验，未提交行为修改），但 `sim_get_test_state` 仍然在 cycle 3 返回
`1`（win）。这说明关卡测试的输出判定不是通过这两个 UI 读取入口完成的，
而是在仿真/JIT 线程内部直接完成的。

结论：路线 1 不能只 Hook `sim_state_read_u64`/`get_sim_state` 来改变逻辑；
下一步必须进入仿真线程的每周期路径（`jit_function`/`jit` 包装或生成的
状态写入点），在测试判定读取输出之前写入插件算出的状态。

### 12.3 最小原生自定义逻辑已跑通（2026-09-17）

继续排查发现 `sim_get_cycle` 和 `sim_get_test_state` 都直接读
`simulation_settings`：

- `sim_get_cycle()` → `get_simulation_setting(0)`
- `sim_get_test_state()` → `get_simulation_setting(2)`

`set_simulation_setting__modelZsimulator95types_u118` 可以直接写回这两个值。
于是 `tests/sim-state-probe.cpp` 增加了一个最小原生仿真模式：插件在
`sim_do` 的 run 命令里不调用游戏 JIT，而是自己按周期做：

1. 从 cycle 推出 `and_gate` 测试输入 `U2 cycle`。
2. 用 C++ 计算自定义逻辑（实验用 OR）。
3. 写 `simulation_input_replay`（偏移 0/8）、`simulation_output_history_pins`
   （偏移 55/64）和 `simulation_state`（256/258/264 等映射槽）。
4. 写 `set_simulation_setting(0, cycle)` 和
   `set_simulation_setting(2, win/fail)`。

真机沙箱结果（`TC_SIM_STATE_LOGIC=or`）：

| 观测 | 结果 |
|---|---|
| 输入值 | `0,1,2,3` |
| 输出值 | `0,1,1,1`（OR 真值表） |
| 测试状态 | cycle 1/2/3 都是 `2`（fail） |

对照原生 AND 输出 `0,0,0,1`，这条路径已经能在最小关卡里实现真正的
插件自定义逻辑，而不是只改统计数字。当前范围仍是最小 `and_gate` 板型和
两级电平 IO；下一步需要把“读板子拓扑、计算每周期输入、写回输出”抽成
稳定接口，并扩展到更多元件 kind 与更复杂的连线。

### 12.4 原生逻辑 SDK 与示例 Mod（2026-09-17）

> 状态：**已被 12.5 取代**。本节描述的 `sdk/tc_custom_logic.h` 会接管
> `sim_do`、自己解释整块板子并自行判定关卡，只能算概念验证；它保留在仓库
> 中仅用于 `tests/sim-state-probe.cpp` 研究探针。生产路径见 12.5。

新增 `sdk/tc_custom_logic.h`：

- `TCCustomLogicComponent` 描述自定义 ID、输入/输出数和 C++ 逻辑回调。
- `TCCustomLogicRuntime::run` 解析运行时 board：
  - 元件序列：`model+0x78`/`+0x80`，步长 `0x238`
  - 导线序列：`model+0x98`/`+0xa0`，步长 `0x68`
  - wire 两端 int16 坐标在 `+0x18`/`+0x1c`，运行时状态索引在 `+0x38`
- 运行时对 level input `0x3f`、level output `0x44`、自定义实例 `0x4e`
  建立网表，每周期调用插件回调，并把值写回 `simulation_state` 与
  `simulation_settings`。

新增示例 `examples/custom-or`：内部电路仍是 AND 作为后备，C++ 回调把行为
替换为 OR。`tests/custom-or-playtest.ps1` 已真机通过：

```text
[example.custom-or] custom-or: registered native OR2 id=4705773643784794161
[example.custom-or] custom-or: cycle=1 output=1 expected=0 fail
PASS native custom OR component produced OR behavior through C++ callback
```

这标志着“元件行为由插件 C++ 决定”已经从探针实验升级为可引用的 SDK 能力。
当前限制：只解释简单 level IO + 自定义实例 + 点对点导线；内置逻辑门、
复杂网表、RAM/寄存器和暂停/重置生命周期尚未纳入。

实验步骤（下一轮执行）：

1. 新建 `tests/sim-state-probe.cpp`：导入一个自定义元件 → 载入 `and_gate` 关卡 →
   用 `set_sim_test` + `sim_do(model,0,N)` 逐步跑 4 个周期。
2. Hook `jit_function`。每个周期结束后把 `simulation_state` 全部**按字节**快照
   （上一轮只按 8 字节步长扫，可能漏掉字节级布局），只保留"跨周期发生变化"的下标。
3. 用关卡测试已知的期望序列（`and_gate` 的 `0,0,0,1`）与输入序列做相关，找出
   匹配的下标 → 即输入/输出引脚对应的状态槽位。
4. 用同一方法验证**有状态**元件（Delay Line 设计）：输出应比输入晚一个周期，
   从而确认状态槽位在跨周期保持、暂停、重置时的行为。

拿到稳定映射后：

- 第 5 步：状态保持/暂停/重置直接用"读引脚值 + 差分"验证；
- 路线 1：在 JIT 包装里为插件的元件读写这些槽位，从"改统计"升级为"改行为"。

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
延迟简单加到全局分数上；当时门数仍沿用游戏原来的递归计算（11.4 起改为使用声明门数）。

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

### 11.4 声明门数纳入板级分数（2026-09-17）

用户实测 `examples/byte-adder`（声明 1 门 / 1 延迟）在关卡里的门数不是 1。反汇编
`get_gate_cost__modelZscores_u2560`（0x140159400）后原因很明确：

```text
get_gate_cost(seq, skip_custom)            # seq: 长度 +0、数据 +8、步长 0x238
  for 每个元件:
    if kind == 0x4e:                       # 自定义元件
        if skip_custom: 跳过                 # 「不计自定义」的语义
        else: get_custom_prototype(id) → 递归 get_gate_cost(prototype+0xE0, 0)
    else: get_cost(&pair, 元件) → 累加 pair.gates
```

也就是说这个函数的**第一个参数是元件序列而不是单个元件**，自定义元件按定义里的
内部电路递归展开（`prototype+0xE0` 就是定义的元件序列），根本不会读原型的
`+0x130`。整板分数则由 `preorder_u8749+0x5884` 用 `skip_custom=1` 调用它：编译图里
已经含有展开出来的内部元件，跳掉 0x4e 包装件，于是 `51 = Mux(50) + NOT(1)`。

同时确认声明值在哪一步丢失：真机日志里导入阶段的 4 次注册都是
`pre=(51,1) post=(51,1)`，而插件自己那次注册是 `pre=(1,1) post=(1,1)`——解析定义时
重算门数、保留头部延迟，插件随后写回的 (1,1) 是**最后一次**注册。因此加载器在
`tc_mod_load` 作用域内捕获声明值（该作用域内最后一次注册为准），并在
`get_gate_cost_u2560` 的包装里按下列规则求和：

1. 已登记 Mod 元件的元素按声明门数计入（`skip_custom` 也照计）；
2. 顺着 `+0x18` / `+0x10` 父链能追到已登记 Mod 元件的元素计 0——它们的代价已经在
   声明门数里，嵌套 Mod 同理；
3. 其余元素逐个交给原函数（包装成单元素序列 `{1, 元素指针 - 8}`，注意序列数据指针
   比首个元素早 8 字节），完全保留游戏的 `skip_custom` 语义与错误处理。

反向验证：只用序列包装、把单元素序列的数据指针写成元素本身（漏掉那 8 字节）时，
原函数会读到错位的元件，`byte_adder` 关卡的异步编译直接不产出结果
（`build_scores gates=0 delay=0`、`sim cycle=-1`）。修正后同一用例读数为
`compiled gates=1 delay=1`、`build_scores gates=1 delay=1`。

### 12.5 代码生成桥接：与普通电路共同工作的原生逻辑（2026-09-17）

12.4 的整板解释器不满足“和普通电路一起工作”：它接管 `sim_do` 的 run 命令，
自己推关卡输入、自己判定结果，内置门完全不参与。改为进入游戏自己的代码
生成链路后，这些职责全部还给游戏。

接入点（固定构建 2.1.334，全部经 COFF 符号解析）：

| 符号 | 作用 |
|---|---|
| `add_circuit_code__modelZsimulationZcode95gen_u4263` | 每个元件发射中间代码；第 2 个机器参数是扁平化后的元件序列（长度 `+0`、数据 `+8`、步长 `0x238`） |
| `add_line__modelZsimulationZcode95gen_u2129` | 发射单行源码；行内 `// <序号> <kind 名> ...` 注释给出元件序号 |
| `handle_request_compile_and_run__modelZsimulationZsimulator95functions_u20` | 编译并运行；第 3 个参数指向待编译源码字符串 |

实现（`src/native_logic.hpp`）：

1. 元件序列里 kind `0x4e` 是自定义实例，其内部节点带 `+0x18` = 实例 ID。
   实测一个实例的内部节点是 2 个 kind `0x50`（输入引脚 helper）加 1 个
   门节点。只有**恰好一个**一位逻辑门（kind `0x03`–`0x0b`）且该门为两输入
   AND/OR/XOR（`0x04`/`0x07`/`0x0a`）的实例才会被桥接；门多于一个或形状
   不被支持时保留内部电路并写日志。首次编译会打印一次
   `Native logic: board layout node<i>=<kind>/parent=<instance> ...`。
2. `add_line` 里按注释序号定位该节点的值行，把 `var vidN = U1 A & B` 这类
   表达式替换为
   `U1 game_engine.'tc_logic_invoke'(U64 <token>, U64 (cycle + 1), U64 (A), U64 (B))`；
   refresh 模式用 `tc_logic_peek` 且传 `cycle`。操作符识别按括号深度取
   `&`/`|`/`^`，允许一层 `!(...)` 包装；两个操作数必须是单一值。
3. 编译前在源码头部补 `extern windows_x64 game_engine`，把
   `tc_logic_invoke`/`tc_logic_peek`/`tc_logic_reset` 注册进 JIT foreign
   library 表，并在 `def reset_sim()` 开头插入 `tc_logic_reset()`。
4. token ↔ 实例绑定在编译期建立，key 是 (custom_id, 实例 ID)，重编译复用；
   每个绑定有独立 `state[8]`，只有 CYCLE 阶段提交，REFRESH 用副本，
   RESET 先把 state 归零再回调。

内部节点操作数不参与语义：回调收到的是该节点两个输入线上的值，返回的值直接
成为该元件的输出，因此元件内部电路退化为“接线”，行为完全由插件决定。

#### 12.5.1 真机验证（三场景）

`tests/native-logic-playtest.ps1` 在隔离副本与存档中运行，断言全部来自游戏
自身（关卡判定、关卡输出引脚历史 `output_history_pins`、每实例回调计数、
编译结果里的关卡 IO 元件数量）：

| 场景 | 板子 | 关卡 | 引脚历史 | 判定 |
|---|---|---|---|---|
| `or` | 输入 → Mod → 输出 | `or_gate` | `0,1,1,1` | win |
| `mixed` | 输入 → Mod → 内置 NOT → 输出 | `nor_gate` | `1,0,0,0` | win |
| `multi` | 输入 → 两个 Mod 实例 → 内置 AND → 输出 | `or_gate` | `0,1,1,1` | win |

`multi` 场景同时给出：实例 `0x2222…`/`0x5555…` 分别绑定 token 1/2，各自
每周期恰好调用一次（合计 8 次/4 周期），重置时两个实例都收到回调。
`mixed` 场景的关卡期望是 NOR，只有“回调算 OR、内置 NOT 再取反”同时成立
才能通过，因此它同时证明内置门仍由游戏执行且与回调处于同一条编译链。

#### 12.5.2 两个已确认的坑

1. **原理图不要复制关卡 IO 元件。** 夹具初稿把关卡自带的 `0x3f`/`0x44`
   也写进了原理图，游戏把两部分合并，板上出现两个输出元件。关卡判定
   `check_output` 使用最后发射的那个 `level_output`，而界面表格读引脚历史
   中对应槽位的那一个；两者不同就会出现“当前输出与期望表不符却通关”。
   夹具现在只含门电路，回归脚本断言编译结果里恰好一个关卡输入和一个关卡
   输出元件。
2. **引脚偏移不对称。** 自定义实例的两个输入是 `(-1,-1)` 和 `(-1,0)`，输出
   是 `(2,-1)`（相对元件坐标），不是对称布局；注册日志会打印实测值，写夹具
   或布线前先看这条日志。

### 12.6 接口形状泛化（2026-09-17）

12.5 只桥接「两输入一输出 + 内部一个门」。现在形状由**元件定义**决定，
运行时按定义派发；ABI 升到 2（`TCLogicIO` 带 `input_count`/`output_count`、
`inputs[8]`、`outputs[8]`）。

机制调整：

1. 一个实例的内部门（kind `0x03`–`0x0b`）数量必须等于该定义的输出脚数：
   每个输出脚由一个门驱动。门多于输出脚（多级内部逻辑）时保留内部电路并写诊断。
2. 回调输入元组 = **第一个输出门**的操作数列表：run 模式取该行引用的变量
   （`vidNNN`），refresh 模式取 `load(...)` 子表达式；数量必须等于定义的输入脚数。
   这样 NOT 之类一元门、三输入门都能用，且不必解析运算符。
3. 第一行替换为 `tc_logic_invoke`/`tc_logic_peek`，其余输出脚的行替换为
   `tc_logic_out(token, k)` 回读；`tc_logic_out` 读的是上一次 invoke 保存的输出。
4. 输入按位打包进一个 U64 传给生成代码。最初实现直接展开成最多 6 个参数，
   游戏编译器立即报 `register_frame.nim(119,3) register_slots[index] == EXP_NULL`
   断言（foreign 调用超过 4 参数的路径不安全），改成打包后保持已验证的 4 参数形状。

定义序列化的两个实测约束（用游戏自带 v14 元件文件 `Not ZR`／`Conditions`／
`ALU`／`Instruction Decoder` 对照）：

| 约束 | 现象 | 依据 |
|---|---|---|
| 多脚引脚几何 | 三脚定义沿用两脚夹具的两格间距时导入失败；改成左列间距 8（`(-18,-10)`,`(-18,-2)`,`(-18,6)`）、输出右侧间距 8 后导入成功 | 游戏 ALU 定义与 7 输出 Instruction Decoder 的引脚坐标 |
| v14 组件尾部顺序 | `bool_a, value_i64_a, value_i64_b, bool_b` 顺序写错（写成 `-256/255`）时三脚定义被拒；按游戏顺序（`value_i64_a=-1`、`value_i64_b=0`）后通过 | 上述文件的逐字段对比 |

真机场景（`tests/native-logic-playtest.ps1`，断言全部来自游戏自身）：

| 场景 | 元件形状 | 关卡 | 结果 |
|---|---|---|---|
| `shape1` | 1 进 1 出（NOT） | `not_gate` | win；引脚历史 `1,0` |
| `shape3` | 3 进 1 出（AND） | `and_gate_3` | win；输入元组保序，8 周期 |
| `shape32` | 3 进 2 出（全加器） | `full_adder` | win；关卡同时校验 Sum 与 Carry |

实测引脚偏移：两脚 `in0=(-1,-1) in1=(-1,0) out0=(2,-1)`；
三脚 `in0=(-2,-1) in1=(-2,0) in2=(-2,1) out0=(2,0)`；
三脚两出 `out1=(2,1)`。偏移由游戏计算，注册日志会打印实测值。

### 12.7 多位宽引脚（2026-09-17）

在第 12.6 的泛化之上，引脚位宽也放开到 1–64 位（总输入 ≤128 位）。实测确定的
两条 JIT 约束决定了实现方式：

| 实验 | 结果 |
|---|---|
| `tc_logic_invoke(token, cycle, i0, i1, i2, i3)` 六参数外调 | 编译期断言 `register_frame.nim(119,3) register_slots[index] == EXP_NULL` |
| 外调返回值按 `U1` 消费（原 1 位路径） | 正常 |
| 外调返回值按 `U8`/`U64` 消费并写入字宽关卡输出脚 | 关卡读到的值渲染为空、判定 fail；改写成常量后同一位置判定 win，说明赋值通路正常、是取值通路的问题 |
| 回调把位写进 `simulation_state` 预留槽，生成代码 `load(<U1>, #SIMULATION_STATE + n)` 组字 | 关卡 16 轮字节比较全部通过，判定 win |

因此字宽输出的发射形式与游戏自己的字宽元件（`com_maker_bit_8`）一致：
逐位 `load` + 移位或；输入则按引脚宽度打包进两个 payload 字，保持 4 参数外调。

真机证据（`double_number`，8 位输入 / 8 位输出，16 轮随机字节）：
`registered ... in0=(-2,0,w8) out0=(2,0,w8)`、`cycle=0 in=[81] out=[162]`、
`run finished cycle=15 ... verdict=1`，关卡 UI 期望值与当前值都渲染为 162。

仍未支持：内部多级逻辑、RAM/寄存器内部电路、暂停语义的单独验收；字宽输出占用
状态缓冲区 `0x9a0000` 起的预留槽（32 个实例 × 512 字节），超大型板子理论上可能冲突。

### 12.8 位宽/形状组合扩展与两个夹具陷阱（2026-09-17）

内部门识别范围从"1 位门"扩到 `0x03`–`0x0b`、`0x12`–`0x1e`（字宽逻辑与算术比较）
以及字宽 Mux `0x2a`，因此"每个输出脚一个门"的规则也适用于字宽元件。

新增真机场景（`tests/native-logic-playtest.ps1`，共 11 个）：

| 场景 | 形状 | 关卡 | 关键点 |
|---|---|---|---|
| `shape_xor8` | 2×8 位 → 8 位 | `byte_xor` | 两个字宽输入 |
| `shape_mux8` | 3×8 位 → 8 位 | `byte_mux` | 三操作数元组（24 位输入） |
| `shape_asr8` | 8 位 + 3 位 → 8 位 | `byte_asr` | 混合位宽输入 |
| `shape_adder8` | 1+8+8 → 8+1 位 | `byte_adder` | 两个方向都混合位宽、双输出 |

`byte_*` 关卡的胜利条件需要 0xffff/0x1ffff 个周期，故采用"上限周期内无失配
（`verdict=0`）+ 测试脚本解析回调日志独立复核目标函数"的双重断言。

两个夹具陷阱（本轮各犯一次，诊断日志都能直接指出）：

1. **组件数与实际节点数不一致**：adder8 定义写成 `i64(6)` 却有 7 个组件，游戏
   仍能"导入成功"但按错位解析，原型只报 1 个输出脚、引脚几何也是错的。修完
   计数后恢复正常。生成定义文件后应解码一次做交叉检查。
2. **连线少走/多走一格导致引脚悬空**：Mux 定义的三条输入线用 `0x000F + 0x0001`
   才等于 16 格，写成 `0x0010 + 0x0001` 就多走一格。表现是实例被绑定但回调从不
   执行，发射行退化为常量表达式 `(U1 0x0) & 1 == 0 ? (U8 0x0) : (U8 0x0)`；
   新加的 "exposes 0 operand(s)" 诊断把该行原样打印出来，据此定位。

仍未验收：16/32 位关卡（symphony 系列的 32 位 IO 判定依赖 RAM 与指令执行）、
字宽状态保持（计数器类）、暂停语义。
