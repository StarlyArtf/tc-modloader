> **研究日志（非发布文档）。** 本文记录一次“把不确定语义收口”的过程与证据。
> 面向使用者的结论在 [sdk/services.md](../sdk/services.md) 与
> [sdk/game-model.md](../sdk/game-model.md)。

# 棋盘对象字段与引脚描述符

日期：2026-09-20。Board V4 故意留了两个问题没回答：元件记录里的实例父链
（`+0x10`／`+0x18`）到底是什么，以及引脚描述符该从哪里开始读。本文把这两件事收口。

## 1. 实例父链不是板级字段

**问题**：`src/component_timing.hpp` 与 `src/native_logic.hpp` 都会读元件记录的 `+0x18`
（研究文档写作“实例 ID”），并沿 `+0x10` 继续向上走。那 Board V4 是否应该把它暴露给 Mod？

**方法**：把两次真机 dump 的板级记录按字节摊平，看这些偏移在板上到底有没有内容：

- `build/state-map.txt`：sim-state 探针在 fixture 导入的 and_gate 板上 dump 的整张表；
- `game-handle-probe` 的棋盘 dump：战役关卡，两条独立记录。

**证据**（非零字节按偏移列出）：

```text
COMP 1 kind=0x3f pos=-13,0 id=0x1111111111111111  +0x00 +0x02 +0x08..+0x0f
COMP 2 kind=0x04 pos=-5,0  id=0x2222222222222222  +0x00 +0x02 +0x08..+0x0f +0x30 +0x39..
COMP 3 kind=0x44 pos=13,0  id=0x3333333333333333  +0x00 +0x02 +0x08..+0x0f +0x30 +0x38..
```

`0x3f` 是 fixture 里那个带名字 `"Input"` 的 2 位输入引脚，`0x44` 是带名字 `"Output"` 的
1 位输出引脚。`+0x10`／`+0x18` 对它们**全为零** —— 如果板级记录承载父链或名字，这两个
字段至少不该同时为空。

**结论**：读这两处的代码都跑在**编译期扁平序列**上，而不是板块记录：
`src/component_timing.hpp` 的 `prepare()` 从编译图 `+0x60`／`+0x68` 取序列，
`src/native_logic.hpp` 的桥接跑在 `add_circuit_code` 的扁平元件序列上（研究文档 §12.5）。
“这个内部节点属于哪个实例”是扁平化时写进副本的，板块自己没有这项信息。

**对 API 的影响**：Board V4 不暴露这两个字段是对的。真要暴露，它属于另一个服务
（编译图／代码生成观测），捕获点与生命周期契约都不同——加载器已经握着 `preorder` 与
`add_circuit_code` 两个挂钩点，但那是下一条路线。

## 2. 引脚描述符的锚点是 `+8`

**问题**：`docs/sdk/game-model.md` 写“相对坐标在描述符 `+2`，描述符起点为 `TCPin* + 8`”，
但 SDK 的 `prototypeInputPin()` 返回的正是 `TCPin*`，于是每个调用方都要自己做这个加法
（`tests/kind-list-probe.cpp` 之前就带着一行手工 `+8`）。

**方法**：让探针用这套约定 dump 真机全部 125 个内置原型的引脚，再和独立核实的引脚偏移表
（`component-pipeline.md` §9）逐条对照。

**证据**（`build/kinds.txt` 摘录）：

```text
0x4  name="AND"    inputs=2 outputs=1 in0=(-1,-1,w1) in1=(-1,1,w1) out0=(2,0,w1)
0x12 name="NOT"    inputs=1 outputs=1 in0=(-1,0,w…)  out0=(2,0,w…)
0x44 name="Output" inputs=1 outputs=0 in0=(-1,0,w1)
0x31 name="Count leading zero" in0=(-1,0,w9223372036854775807) out0=(2,0,w…)
```

`w1`／`w8` 与 `0x7fffffffffffffff`（AUTO_SIZE）说明 `TCPin* + 0x10` 就是原始 WordSize；
坐标与 §9 的表格逐条吻合（AND 输入 `(-1,±1)`、输出 `(2,0)`；Output 输入 `(-1,0)`）。

**结论**：布局本身早就核实了，缺的只是封装。本轮把锚点算术收进 SDK
（`tc::pinPoint()`、`tc::prototypeInputPinPoint()`／`OutputPinPoint()`、
`tc::prototypeInputPinWordSize()`／`OutputPinWordSize()`、`tc::pinWordSizeIsAuto()`），
并把探针改成只用这些入口。真机重新 dump 的结果与改动前**逐行一致**（126 行、125 个元件，
名字／引脚数／偏移／字宽全同）——这是封装正确性的真机证据，不是推断。

## 3. 本轮没有回答的

- 引脚条目里除「相对坐标」与「WordSize」之外的字段（名字、方向、ordinal 等）。
- 实例父链若要暴露，需要一个新的编译图观测服务，以及它自己的捕获点、线程与失效规则。
- 编译图／代码生成层的其它可观测量（扁平序列、代价、时序）同理：它们属于那条路线。

## 4. 导线记录是"两个端点"，而加线入口只起一条退化的线

### 4.1 端点读法被实验推翻

原来的读法把 `+0x18`／`+0x1c` 当成"一个端点的 x、y"。P1 想加导线编辑时做了两个实验，两个都指向
另一种解释：

1. 真机放一条线（`add_wire_from_pos(board, point, colour)`），记录里出现的不是"一个点"；
2. 回头核对 `build/state-map.txt` 里 and_gate 内置解的导线字节：`+0x18` 起是
   `faff ffff f3ff ffff` = `(-6,-1)` 与 `(-13,-1)`——一条**水平**线，两个端点的 y 都是 -1。

按"两对 int16"重读战役关卡的导线，得到 `(9,0) -> (-9,0)`：那条线正好横在 (-10,0) 的输入引脚与
(10,0) 的输出引脚之间。几何自洽，读法确定：

```text
+0x18 i16 x1   +0x1a i16 y1   +0x1c i16 x2   +0x1e i16 y2
```

`TCWireInfoV1` 已改为 `x1/y1/x2/y2`（原来导出的 `x/y` 其实是 x1 与 x2，纯属误导）。

### 4.2 为什么命令总线还没有加线

`add_wire_from_pos__modelZboardZboard_u28435` 只需要 board + 点 + 颜色，看起来和元件放置一样
理想。真机实验（在 fixture 板上放一条线，再用游戏自己的 `get_wire(model, point)` 回查）：

```text
component placement wire board before=0 after=1 endpoint=10,10 found=1
component placement wire list w(10,10,w1,slot1)
component placement wire query invalid=-1 at(10,10)=-1 at(0,0)=-1
```

- 棋盘上确实多了一条记录，两个端点都等于请求的点（零长度）；
- 但 **游戏自己的点查询在它自己的端点上返回 `INVALID_WIRE_ID`**；
- 它的状态槽是 `1`（奇数），而真正的线是偶数槽（`256/258/260`）。

结论：这个入口是"鼠标按下开始拉一条线"，线要等 UI 的后续步骤（`handle_update_wire` /
`handle_place_wire`，两者都需要真实的 input 上下文）才变成可寻址的线。把它做成队列命令只会在
玩家棋盘上留下半成品导线，所以**故意不进命令总线**：契约里没有 `PLACE_WIRE`，
`docs/sdk/commands.md` 写明原因。

要做真正的程序化连线，需要下面之一（都还没有证据）：

1. 找到"按两个端点建线"的低层入口并核实；
2. 让加载器接管 `handle_update_wire`／`handle_place_wire`（链式钩子模式），并且**只在真实回调
   栈内**执行编辑——那就不是队列命令的语义了，得另设计一条"回调内编辑"的通道。

这条决策留给下一轮：它要么需要新的反推，要么需要一次兼容性取舍（把 wire-palette 迁到链式钩子）。

## 5. 选择集写入的候选入口（线索，签名未核实）

用 `nm "Turing Complete.exe"` 在 `modelZboardZboard` 命名空间里筛出来的候选，**都还没做签名核实**
（下一步要反汇编 + 真机探针，像 `tools/Inspect-Components.js` 那样先看参数布局再动手）：

| 符号 | 猜测 | 备注 |
|---|---|---|
| `select_component__modelZboardZboard_u9202` | 选中一个元件 | 最直接的候选；需确认参数是 (board, component_id) 还是 (board, 组件记录指针) |
| `incl__modelZboardZboard_u17136` / `incl__modelZboardZboard_u2245` | Nim HashSet 的"加入" | 两个候选，很可能是元件集合与导线集合各一个；需确认第一个参数是集合还是 board |
| `excl__modelZboardZboard_u21312` | Nim HashSet 的"移出" | 同上 |
| `clear_selections__modelZboardZboard_u8323` | 清空两个集合 | 参数最少、最容易先试 |
| `get_selection__modelZboardZboard_u9061` | 取当前选择 | 与 SDK 已有的 `contains`／`len` 查询互补 |
| `add_undo_changes__modelZboardZboard_u23805` | **把变更推入撤销栈** | 与"让 Mod 的编辑和玩家 Ctrl+Z 共存"直接相关：如果签名可核实，Mod 也许能让一批编辑成为**一步**撤销，而不必自己实现逆操作 |

同一次 `nm` 还确认了选择集本身的两个全局量（SDK 已在用）：`selected_components__modelZboardZboard_u22`
与 `selected_wires__modelZboardZboard_u30`，以及 `prev_selected_*` 两份上一帧快照。

**注意**：这些名字只能说明"这里有这么个函数"，不能说明它的参数、所有权或副作用。按本仓库的规矩，
在真机探针把行为打出来之前，它们不进任何契约。

## 6. 删除入口已核实；移动入口还只是“提交拖拽”

### 6.1 `board_delete_component` 的参数与真机行为

反汇编 `board_delete_component__modelZboardZboard_u10711` 及游戏自己的
`try_delete__modelZutilities_u8585` 调用点，参数已经可以确定为：

```cpp
void board_delete_component(void* component_sequence, int64_t index);
// component_sequence == static_cast<unsigned char*>(board_model) + 0x78
```

入口先从当前／上一帧选择集中移除该 index，再处理 `board + 0x78` 指向的元件序列。真机探针按
“放一个自定义元件 → 删除最后一个 index → 只用 Board V3/V4 回读”验证：

```text
component deletion candidate before=2 after=2 live=1->0 found=0
```

`found=0` 说明原坐标 `(30,0)` 已查不到，活元件数少一个；序列长度仍是 2，因为删除把记录变成
kind 0 墓碑，而不是缩短数组。紧接着放置命令把墓碑槽复用，读数是 `before=2 after=2 live=0->1`。
所以今后所有“元件数”断言都必须区分**序列长度**和**跳过 kind 0 后的活记录数**。

这个低层入口本身没有看到 `add_undo_changes` 调用，暂时只算“删除原语已核实”，还没有直接进命令
总线。反汇编同时找到了更合适的撤销感知包装：

```cpp
bool try_delete(void* board_model,
                const NimSeq<int64_t>* component_indices,
                const NimSeq<int64_t>* wire_ids);
```

它先复制要删的记录，调用 `board_delete_component`／`board_delete_wire`，最后把生成的变更交给
`add_undo_changes`。下一步应给这个包装写真机探针（删除 → undo → redo），通过后再设计命令载荷；
在这一步之前不把“可撤销删除”写进契约。

### 6.2 为什么 `board_commit_move` 还不能接

`board_commit_move__modelZboardZboard_u24175` 的 Windows x64 调用布局是隐藏返回缓冲在 `rcx`、
`board + 0x78` 在 `rdx`。它遍历全局 `selected_components`，复制**已经由拖拽过程改过坐标**的记录，
返回变更列表；唯一调用者随后才调用 `add_undo_changes`。也就是说它只是“提交先前拖拽”，没有目标
坐标参数，不是一个可以独立调用的移动入口。

同批符号里还确认了 `handle_move_selection`、`stash_move` 与 `select_component`，但它们依赖输入上下文、
选择集或拖拽状态。按计划边界，本轮只记录签名线索，不实现“让游戏选中”；移动也继续留在探针阶段。
