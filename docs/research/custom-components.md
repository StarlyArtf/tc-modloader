# 自定义元件（kind 0x4E）在沙盒里怎么跑起来

> **研究/交接日志（非发布文档）。** 记录"让 simcore 兼容自定义元件"这件事量到了什么、
> 设计是什么、还差什么。面向使用者的结论从 [文档索引](../README.md) 进入。

日期：2026-09-30。构建：`Turing Complete.exe`（该指定构建）+ 本仓库的 simcore。

## 1. 一句话结论

自定义元件（kind `0x4E`）在游戏里不是一个器件，而是一份**定义**：游戏把一份电路解析后
放进它自己的**自定义原型表**。原型里就带着那份电路——组件数组在 `+0xe0`（数量）/`+0xe8`
（数据），导线数组在 `+0x100`/`+0x108`，元素格式与棋盘记录**完全一样**。沙盒因此在构建
网表之前把每个自定义实例**展平**：把定义里的内部元件与导线搬到一个不会撞到任何东西的
远端坐标，再按**序号**把它自己的引脚与实例的引脚接起来。落地之后，玩家那块带 5 种
自制元件的 CPU 板从"整块拒跑"变成 **nets=130 / devices=103** 的正常网表（§4）。

## 2. 之前为什么拒跑

两处，先后暴露：

1. **引脚上限**：`examples/sandbox-sim` 的几何回调写着 `inputs > 8 || outputs > 4` 就拒绝。
   玩家的 `Symphony/DEC` 是 **1 进 7 出**，于是日志里是
   `component 7 has kind 0x4E with no pin geometry`。这个上限原本是照"内置元件最宽八个叶子"
   定的，对自定义元件不成立（一个 5→32 译码器有 32 个输出）。现在 `KindPins` 每侧 64 个脚。
2. **没有行为**：即便几何拿到了，之前 simcore 对不认识的 `0x4E` 只有一句
   `has no sandbox simulator behavior`（fatal）。因为它不知道这个元件**是什么电路**。

## 3. 定义在哪里：自定义原型里的两个数组

探针 `tests/custom-prototype-probe.cpp`（跑法：`tests/custom-prototype-playtest.ps1`）
把游戏自己的自定义原型整块 dump 出来，跟随两个计数数组的指针。以玩家的
`Symphony/DEC`（1 进 7 出）为例：

```text
# component count=15 ptr=0x13dc311f030 stride=568
component[0] ... 51 00 02 00 ec ff 00 00 <id> ...      <- Output Pin, x=2,  y=-20
component[7] ... 4f 00 f5 ff fc ff 00 00 <id> ...      <- Input Pin,  x=-11,y=-4
component[8] ... 65 00 fc ff 1c 00 00 00 <id> ...      <- Static Indexer, x=-4,y=28
# wire count=20 ptr=0x13dc2e2f040 stride=64
wire[0] ... f8 ff 0c 00 f8 ff 14 00 ...                <- (-8,12) -> (-8,20)
```

量到的布局（`tools/definition-layout.py` 用来复核，`build/kinds.txt` 提供每种 kind 的
引脚数）：

| 项 | 值 |
|---|---|
| 组件数 / 数据 | 原型 `+0xe0` / `+0xe8` |
| 导线数 / 数据 | 原型 `+0x100` / `+0x108` |
| 元素起点 | 指针 `+ 8`（Nim 序列自己的头在数据前面） |
| 组件元素 | 步长 `0x238`，与棋盘记录同形：kind `+0`、x `+2`、y `+4`、rotation `+6`、id `+8`、custom `+0x188` |
| 导线元素 | 步长 **`0x68`**（与棋盘记录同一套布局），两端点 `+0x18..+0x1e`（四个 int16）、腿数 `+0x20`、腿指针 `+0x28` |

**导线步长那次量错了，2026-09-30 更正**：探针当时把 `0x40` 当成步长，于是"每隔一条"记录
落在前一条的中间，内部连线大半读成 `0,0 -> 0,0`。照真机内存重量：腿指针（`+0x28`）每
`0x68` 出现一次，端点每 `0x68` 落在 `+0x18`。改回 `0x68` 之后，7 份 foundry 定义的 248 条
内部导线**没有一条**再读成退化值；玩家那块板子从 130 网/104 器件 变成 **183 网/146 器件**
——以前"少"的那些内部连接根本没接上。

**定义导线不带宽度**：棋盘导线把宽度写在 `+0x30`，定义导线那里 248/248 全是 0。宽度得从
**引脚**来：引脚自己声明的宽度，或该元件记录里游戏解析出的 `+0xe0`（声明为 `wauto` 时）。
`board.hpp` 现在的顺序是"网络自己的导线宽度优先，导线不带宽度时才用它上面引脚的宽度"，
否则自定义元件内部每条 32 位通路都会被建成 1 位网络。

**嵌套定义必须各占一块地**：一份定义里可以再放一颗自定义元件，而那份定义的内部件往往比
实例占的格子大得多——早先按"父定义的坐标系"展平，它会**盖在父定义其它元件上**，凭空造出
短接。玩家板子上那 6 条 `component pins 2 share the cell …` 就是这么来的（其中一条是四分线器
的 `out3` 压在另一份定义里取负器的 `out0` 上，属于两份**不同**定义）。现在每个被展平的实例
（含嵌套的）都分到自己那块 200 格区域，桥接线把实例引脚连到它自己那份电路；区域总数超出
int16 能容纳的范围时**拒绝展平**并如实报出。改完那 6 条 note 一条不剩。

**一处刻意的偏差**：定义里的组件记录**不重复引脚数**（`+0x30`/`+0x40` 读出 0），
而棋盘记录会写。沙盒因此从**原型表**取引脚数（内置 kind 查内置表，自定义 kind 查自定义
表），与几何回调用的是同一处依据，也正是构建器交叉校验时比对的那个数。

## 4. 展平：怎么接进网表

`examples/sandbox-sim/plugin.cpp` 的 `flattenCustomComponents()`：

1. 板级导线原样保留（它们的**状态槽**是游戏要读回去的那些字节）；
2. 每个非宿主自定义实例（宿主件指 `local.clock` 的 `CLOK_001`/`SWIT_001`/`BUTN_001`，
   它们是源，按老规矩保留）：
   - 读定义，把内部元件与导线整体平移到 `x += 100000 + 200 × 实例序号`（**远离**棋盘
     坐标，保证不会与任何东西撞格），内部导线与桥接线的状态槽置为哨兵
     `tcsim::kNoStateSlot`（游戏里没有这些网的字节，写进去就是污染别人的槽）；
   - 定义里的**输入/输出脚元件**（`0x4f` Input Pin / `0x51` Output Pin）按数组顺序就是
     引脚顺序（`ADDRDECODER-3` 的三个输入脚自上而下与 in0/in1/in2 逐条对上），
     于是第 k 个输入脚接实例第 k 个输入脚，第 k 个输出脚接实例第 k 个输出脚；
   - 桥接就是一根新导线，两端正好落在实例的脚格与定义边界脚的格上——棋盘原有的外线
     已经停在实例的脚格上，两根线共享端点即同一个网络，因此**不需要改任何外层接线**；
   - 嵌套自定义元件递归处理，深度上限 8。
3. 读不到定义、或定义的边界脚数与实例不符时**不展平**，保留原记录，让构建器照旧报
   `has no sandbox simulator behavior`；沙盒另外写一行
   `custom 0x… has no readable definition, so it cannot be simulated`，把原因说到位。

## 5. 验证

- 真机 `tests/sandbox-custom-playtest.ps1`（目录条目 `sandbox-custom`）：把玩家自己的
  `schematics/architecture/Symphony/circuit.data`（含 5 种自制元件）与
  `schematics/foundry/**` 复制进隔离存档，进沙盒，断言
  **不再出现 `cannot run this board`** 且绑定成功：

  ```text
  sandbox-sim: bound board -> nets=130 devices=103 sources=3 observed=3 cycles=0
  sandbox-sim: devices -> u3_load32, …, u147_wordsplit32, u140_screen, …
  ```

  同一块板子在这一轮之前是 `component 7 has kind 0x4E with no pin geometry`。
- 回归：`sandbox-storage`、`sandbox-sim`、`sandbox-sim-ring` 全绿；离线
  `simcore` 358/358、`simcore-view` 188/188。

## 6. 还没做/没说死的地方

| 项 | 现状 |
|---|---|
| 展平后的**数值正确性** | 本轮证明的是"能建出网表并跑"，还没有做"同一块板子上 simcore 的值与游戏逐周期一致"的对照。定义里的内部导线宽度取自定义记录（`+0x30`），若某类定义的宽度字段另有出处，会在这一步暴露——现有的**交叉验证手法**（棋盘读回 + VCD 对照）可以直接拿来用 |
| Mod 注册的**原生**元件（`local.float-ops` 等 65 个类型） | 它们的"定义"不是电路而是 Mod 的 C++ 回调。展平对它们不适用，沙盒目前会把它们当未知件报出来；要跑起来需要"让宿主以给定输入调用该 Mod 的回调"这条链路（加载器已经有这套派发，缺的是给沙盒的入口） |
| 缺 Mod 的定义 | 记录还在、定义不在（Mod 被卸载）时读不到定义，按上面的规则报出来，不猜 |
| 查看器（2026-09-30 已补） | `sandbox-sim-board.txt` 仍写**原始**棋盘，所以查看器画的是玩家看到的那张图；但记录现在同时写出每个元件的 `inpins/outpins` 与每份定义的 `defcustom`/`defcomponent`/`defwire` 段，查看器用**同一个** `simcore/include/tcsim/custom.hpp` 展平后再建网表，两条路不再分叉。展平只作用在"喂给 builder 的那份副本"上，原始坐标不被污染；实例保留自己的槽位，所以按棋盘下标的绘制与器件表仍指同一个元件 |

## 7. 相关文档

| 文档 | 内容 |
|---|---|
| [simcore-kind-coverage.md](simcore-kind-coverage.md) | 内置元件的覆盖表（§7 存储家族） |
| [COMPONENT-PIPELINE.md](component-pipeline.md) | 原型表、`get_custom_prototype` 与代码生成链路 |
| [../PLAN-sandbox-simulator.md](../PLAN-sandbox-simulator.md) | 沙盒总计划（S1–S5）、红线 6 |
| `tools/definition-layout.py` | 复核定义数组里字段偏移的脚本 |
