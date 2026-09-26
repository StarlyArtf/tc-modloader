# 接手：元件接口（阶段 1 前半：类型目录）

计划书是 [PLAN-custom-components.md](PLAN-custom-components.md)：本文只记录**这一轮实际做了什么**、
证据在哪、下一块从哪开始。

## 这一轮的范围与结论

阶段 1 原本一句话里包含两件事：**类型注册表/查询** 与 **Pin/Signal V2（可变长引脚与信号）**。
这一轮只做前者，因为后者的三个验收形状（0 输入源、0 输出汇、>64 位信号）都卡在同一个还不知道
答案的问题上——见下面"下一块的待解问题"——按计划的"证据先于契约"，不该先冻结接口。

做完的：

- `TC_SERVICE_COMPONENT_REGISTRY`（`tc.component.registry` V1）：`count` / `get` / `find` / `pin`，
  见 [docs/sdk/services.md](sdk/services.md#tccomponentregistry这次会话注册了哪些元件类型)；
- 两条注册路径（`register_logic`、`register_component`）都写目录，**被拒绝的也写**，并保留第一条
  拒绝原因；注册成功会替换同一 id 的记录；被拒绝的 Mod 的类型会被删除；
- 声明式注册的引脚名 + 桥接读到的真实位宽合流（`noteShape` 只更新形状，不动名字与代价）；
- 示例：`examples/mod-inspector` 的 "component types" 一节；
- 测试：fast 层 `tests/component-registry.ps1`、game 层 `tests/component-registry-playtest.ps1`；
- ABI：`tools/abi-snapshot.cpp` 增加新结构与新常量，基线已更新。

真机一次运行（`tests/component-registry-playtest.ps1`，沙盒 `build/component-registry-reg1`）：

```text
component registry: registering nine inputs returned -2 (refused)
component registry: catalogue: 2 type(s)
component registry: type id=dev.component-registry-probe/0x4241445f53484150 ... active=0 caps=0x0 status="the definition is malformed (name, pins, widths or counts)"
component registry: type id=example.byte-adder/0x414444385f303031 owner=example.byte-adder name="Byte Adder" 3in/2out cost=1 delay=1 active=1 caps=0x7
component registry: pin ... in[0] name="Carry in" bits=1
component registry: pin ... in[1] name="A" bits=8
component registry: pin ... out[1] name="Carry out" bits=1
```

## 下一块（阶段 1 后半）：Pin/Signal V2

要交付的（计划书 §6.2/§6.3）：

1. `TCComponentTypeDefinitionV2`：稳定字符串 `type_id`、分类/标签、实现类型、回调表、
   每方向 0..N 脚、每脚固定/自动/范围位宽、多字信号；
2. 值表示从 `uint64_t[8]` 换成"带位数与字节长度的只读/可写视图"；
3. 旧的 `register_component` / `TCLogicIO` 原样可用（适配层）；
4. 验收：0→1 常量源、1→0 Sink、>8 脚、>64 位信号，离线 + 真机，且 `example.byte-adder` 行为不变。

**待解问题（必须先有答案或明确标记不可用）**：

- **0 输入 / 0 输出的调度** —— **已解决（2026-09-22）**：真机测量见
  [research/custom-component-pins.md](../research/custom-component-pins.md)。结论是两边都靠
  "一个悬空的驱动门"即可：源用"输入悬空"的驱动门、汇用"输出悬空"的假驱动门，游戏自己的编译器
  保留并调度它们（回调每拍一次）。脚手架、`supportedScaffold()` 的 `prefix` 公式与节点数期望
  都已按这个结论改好，离线与真机用例都在。
- **>64 位信号**：现在输入靠生成代码的 4 参数外调（`token, cycle, payloadLo, payloadHi`）塞进两个
  64 位字，合计 128 位；输出靠状态槽 `0x9a0000` 每 token 512 字节按位回读。多字信号要么扩外调
  形状（要先确认 JIT 的寄存器分配上限），要么把输入也搬到状态槽/宿主缓冲，需要一次专门的
  真机测量。
- **>8 脚**：桥接里 `kMaxBridgeInputs=8` 同时影响"输入打包顺序"与"依赖链长度"，需要确认
  依赖链能排多长、以及排序开销是否随脚数线性增长。

建议的下一步：先做一次**只读测量**（阶段 0 风格）：用 `component_definition::encode` 生成
`0 输入 1 输出` 与 `1 输入 0 输出` 两种脚手架，导入游戏，看编译是否通过、回调是否被调用、
以及元件在棋盘上是否可用；把结果写进 `docs/research/`，再决定 V2 的引脚/值模型是否需要
换成"宿主缓冲 + 显式提交"的形式。
