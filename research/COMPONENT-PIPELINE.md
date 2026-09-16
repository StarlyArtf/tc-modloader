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
