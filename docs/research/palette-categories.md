# 右侧元件栏的分类结构（2026-09-26 实测）

玩家要求："把浮点数 mod 的这些元件在游戏内右侧元件栏单独开一个文件夹，也就是浮点文件夹，里面存放
这些元件，而不是都放到自定义元件文件夹。" 这一轮先把"游戏的分类到底是什么"测清楚，结论如下
（探针 `tests/palette-probe.cpp` + `tests/palette-probe-playtest.ps1`，真机只读，完整 dump 在
`build/palette.txt`）。

## 右侧元件栏是什么

- 右侧那一列按钮是**游戏自己的固定分类**，标签来自本地化：`布尔 / 整型 / 杂项 / 输入\/输出 / 自定义`
  （英文 Bit / Word / Misc / IO / Custom，翻译文件里带注释
  `# Component category name, at most 6 glyphs, the rest is cut off!`，说明这是游戏写死的分类名，
  不是从存档里读出来的目录）。
- 绘制代码在 `build__presenterZboard95uiZcomponent95menuZmini95tree_u2`（VA `0x1403d4920`）与
  `build_category__…Zmini95tree_u57`（`0x1403cdbd0`）：前者遍历 `context+0xd948/+0xd950`
  的入口序列，按每条记录 `+1` 字节的低三位分派；后者画一个分类按钮（`SPACING`/`ITEM_SIZE`
  来自 `component_menu/common`）。元件工坊里还有另一棵"深树"
  （`build__…Zdeep95tree_u2`、`build_category__…Zdeep95tree_u21/u3758`），是工坊侧的层级菜单。

## 原型里和菜单有关的字段

用 `sdk/tc_game_model.h` 的原型 API 克隆 125 个内置元件并逐个 dump：

| 偏移 | 内容 | 实测 |
|---|---|---|
| `+0x40` | 8 位"类别"值 | `OR=2`、`NAND=3`、`Multiply=3`、`Modulo=5`、`Equal=6`、`Rotate Left=13`、`CLZ=15`、`Push Button=10`、`Input=1+(1<<16)`、`Output=2+(1<<32)`、`Counter=7+(1<<32)`、`Screen/RAM=0` |
| `+0x88`/`+0x98` | 小整数（1/2/4…） | 菜单排序用的次序键 |
| `+0x90` | 另一个次序键 | 与 `CATEGORY_ORDER` 的条目比较 |
| `CATEGORY_ORDER__modelZboardZprototype95list_u21` | `.rdata` 里的 10 个 64 位条目 | `0x2b7d…` 形状的值（不是指针，也不是小整数），排序比较器 `colonanonymous___presenterZutilities_u9863` 用它给元件排菜单顺序 |

**关键结论：`+0x40` 的类别值并不等于右侧栏的分类页**——`NAND` 和 `Multiply` 都是 `3`，却分别落在
布尔页和整型页；`Input`/`Output` 用高位（`1<<16`、`1<<32`）标记"关卡输入/输出"。

## 自定义元件（含 Mod 元件）为什么都在"自定义"

- 游戏的说明书写得很清楚（`translations/Chinese (Simplified).txt` 第 494 行）：
  "对于元件工坊而言，电路图管理器本身也是自定义元件存储层级的管理器。电路图名称决定了元件的名称，
  **文件夹结构则决定了自定义元件在菜单中的层级位置**。" 也就是说玩家自制的元件是**工坊关卡里的
  schematic**，它的层级来自那些文件/文件夹。
- Mod 的元件是加载器用 `add_custom_prototype` 导入的**原生 custom 原型**（kind `0x4e`），没有对应
  schematic 文件，所以它们和玩家自制元件一样落在 `自定义` 页的根部，游戏没有"给某个原型指定
  分类页"的接口。
- 实测：`local.float-ops` 的 22 个原型全在 `custom_prototypes` 里，`+0x40 = 0`、`+0x88/+0x90/+0x98 = 0`
  （见 `build/palette.txt` 的 `# Float Ops components` 段）。

## 所以要"浮点文件夹"只有两条路

1. **加载器加一个元件栏分类（UI 钩子）**：钩住
   `build__…Zmini95tree_u2` 或它上游那个填 `context+0xd948` 入口序列的地方，插入一个自己的分类按钮
   和它下面的元件列表。要复现游戏自己的入口记录格式（每条记录的 `+1` 字节 + 缩略图 + 拖放/放置
   过程），并保证不影响游戏原有的 5 个分类。这是**加载器级的新能力**，不是 Mod 自己能做的。
2. **按游戏的原生方式给元件"文件夹"**：把 Mod 元件写成工坊关卡里的 schematic（文件夹结构决定
   层级）——也就是把假 schematic 部署进玩家存档，这正是 `docs/PLAN-float-components.md` §8.3
   当初明确否决的退路（玩家存档被写入、元件变成玩家所有、可被删除/改名）。

## 菜单树的实测结构（2026-09-26，路线 2 的底稿）

上一轮先交了"Mod 自己的侧栏面板"（路线 1），玩家否掉了它："我希望分类文件夹是真的插进游戏自己的
分类体系。" 于是把游戏自己的菜单树读了出来（`tests/float-menu-probe.cpp`：钩住游戏自己的
`reload_component_menu__presenterZutilities_u14192`，它建完菜单后 dump `build/menu.txt`），
结构如下（真机、真包，只读）：

```
context+0xd948 = 顶级分类数, context+0xd950 = 顶层节点指针数组
节点（0x30 字节）：
  +0x00  byte  变体0/1/2 节点自己的编号（游戏给的，新建分类时是 0xab = 游戏的"无分类"兜底值）
  +0x01  byte  变体标记：& 7 == 2 分类节点、== 1 自定义元件、== 0 内置元件
  +0x08  string 分类名（{len, data}，UTF-8，游戏自己用本地化字符串）
  +0x18  byte  次序/k  （分类节点上是一个字节）
  +0x20  u64   子节点数
  +0x28  ptr   子节点数组；**第 i 个子节点在 payload + 8 + 8*i**
```

实测的一棵树（节选）：

```text
node=… byte0=47  byte1=2 tag=2 name="布尔"      children=11
  node=… byte0=58  byte1=2 tag=2 name="集线"    children=6   ← 分类里还能套分类
  node=… byte0=71  byte1=0 tag=0 id=0x2d        ← 内置元件：+8 是 kind
node=… byte0=74  byte1=2 tag=2 name="整型"      children=9
  … "逻辑" "算术" …
node=… byte0=116 byte1=2 tag=2 name="杂项"      children=3
node=… byte0=129 byte1=2 tag=2 name="输入/输出" children=5
node=… byte0=151 byte1=2 tag=2 name="自定义"    children=6
  node=… byte0=0 byte1=1 tag=1 id=0x4633324144445f31 name="FP32 Add"  ← 自定义元件：+8 是自定义 id
```

## 结论：给自定义原型分文件夹是游戏自己的机制（2026-09-26）

`get_component_menu` 里遍历 `custom_prototypes` 的那段代码，把**原型名字**当路径用：

- 逐字节扫 `'/'`（`0x2f`）切段，最后一段做叶子节点（`movb $0x1,0x1(%rax)`，`+8` 写自定义 id），
  前面的段交给 `add_to_menu_tree__presenterZutilities_u12208` 当父分类逐级走进树；
- 路径的**根**是游戏自己建的"自定义"分类节点（就是上面 `name="自定义"` 那个），所以自定义原型的
  文件夹一定长在"自定义"里面，不可能变成第六个顶级页；
- `add_to_menu_tree` 的名字还要过 `translate()` 和 `parseEnum("settings_hk_menu_" + …)`，所以
  分类名走的是游戏自己的字符串/枚举通路，模组只需要给名字。

**交付**：Mod 注册元件时名字写成 `浮点/FP32 Add`（`examples/float-ops/components.cpp` 的
`namePrefix`，可用 `TC_FLOATOPS_PALETTE` 覆盖）。游戏于是自己建出 `浮点` 分类节点并把 22 个浮点
元件挂进去——分类节点由游戏分配/绘制/销毁，Mod 不改任何游戏内存，不碰玩家存档。上一轮路线 1 的
侧栏面板（`registerPalette` / `paletteDraw` / `placeType` / `freeSpot`）整段删除。

真机门禁 `tests/float-palette-playtest.ps1`：钩 `reload_component_menu` 后 dump 树，断言
`自定义` 里有一个 `浮点` 分类、它的子节点恰好是这 22 个 id、且这些 id 在树里别处都不出现。

## 还没做的：把 `浮点` 提到顶级页（路线 3，未做）

要做成和布尔/整型/杂项/输入输出并列的第六个**顶级**分类，只有一条路：在游戏自己建完树之后**动手
改它的内存**。可行的最小步骤（本轮没做，风险评估：在玩家进程里改 Nim 对象/序列，销毁路径走
ARC，写错就是崩溃或双重释放）：

1. 钩 `reload_component_menu__presenterZutilities_u14192`（第二参数 = presenter context），在
   原函数跑完后读 `context+0xd948/+0xd950`；
2. 取"自定义"节点的子序列，把 `浮点` 节点（游戏已经建好的那个，名字和子节点都对）从里面摘掉；
3. 把顶层序列扩一格并把 `浮点` 节点接上去（`newSeqPayload`/`prepareAdd` 是游戏的分配器入口，
   元素按上面的 `payload+8+8*i` 布局写）；
4. 之后每次菜单重建都要重做，并且要保证旧序列的释放路径不被破坏。

按游戏原生方式把 Mod 元件写成玩家存档里的工坊 schematic（文件夹结构决定层级）仍然不做：会写玩家
存档、元件变成玩家所有（`docs/PLAN-float-components.md` §8.3 已否决）。
