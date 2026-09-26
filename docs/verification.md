# 验证体系与场景清单

所有“已验证”结论都来自三类测试之一，且都在**隔离副本 + 独立存档**中运行：

| 类型 | 位置 | 覆盖 |
|---|---|---|
| 单元／模型测试 | `build.ps1` 内编译并执行 | 游戏对象模型、组件模型、时序图算法、宿主契约（能力表与版本约束） |
| 脚本集成测试 | `node tests/run.js`、`tests/native.ps1`、`tests/saves.ps1`、`tests/setup.ps1` | 包部署/恢复/补丁、原生 Hook、存档隔离、安装器 |

## 浮点元件族 M2 垂直切片（2026-09-25）

M2 的判据是 `docs/PLAN-float-components.md` §6.1、§7 与 §10.2：十进制 → 运算 → 人读结果 的
完整体验，配置按元件保存，外观与交互对齐原版。

**离线层**（`examples/float-ops/build.ps1` 编译执行两份测试）：

```text
PASS float-ops text layer: 93 parse vectors (value and editor diagnostics),
383 format vectors (shortest/scientific/hex), 19935 shortest-round-trip pairs;
illegal text refused, `bits:` exact, -0 preserved, small buffers safe
PASS float-ops M0/M2: five public V2 probes plus the three M2 components; ...
```

语料来自 `tools/float-decimal-vectors.py`：解析期望用精确十进制算术独立算出，显示期望用
最短往返搜索（区间法 + 用精确舍入复核候选）算出，都不是抄 fast_float 或 Ryu 的答案。开发中两边
互相抓过对方的错：oracle 的 `-0`/`-nan`/`+7` 期望与“最近候选”选择错了三处，C++ 侧最短形式的
科学计数法重复写了负号。

`tests/float-compat.cpp` 还把三个元件当作游戏注册的定义来驱动：常量按配置的 bits 输出；
`3.5 + 1.25 = 4.75` 且 flags 为 0；配置里写 RTZ / RUP 后，半 ULP 用例 `1.0 + 2^-25` 分别得到
`0x3F800000` / `0x3F800001` 且都置 NX；Display 的回调把 `4.75 0x40980000` 写进自己的缓存，
RESET 之后缓存清空。

**真机层**（`tests/float-ops-playtest.ps1`）：棋盘是两个常量 → 加法器 → 显示器。驱动用
`tc.component.storage` 写配置（与编辑器同一条路径），再读显示器自己的文本：

```text
float-ops M2: constant/add/display registered; render=ok geometry=ok storage=ok
instances=ok ui=ok mouse=ok
float-ops M2: display 0x1000000000000003 shows 2 0x40000000 (bits=0x40000000)   <- 默认 1.0+1.0
float-ops M2 test: 3.5 and 1.25 written to the constants (ok)
float-ops M2: display 0x1000000000000003 shows 4.75 0x40980000 (bits=0x40980000)
float-ops M2 test: halfway operands and RNE written (ok)
float-ops M2: display 0x1000000000000003 shows 1 0x3F800000 (bits=0x3F800000)
float-ops M2 test: rounding box set to RUP (ok)
float-ops M2: display 0x1000000000000003 shows 1.0000001 0x3F800001 (bits=0x3F800001)
PASS float-ops M2 vertical slice: constant configuration -> FP32 Add (RNE then RUP) ->
display text; 4.75 and the halfway case 1.0+2^-25 both reached the board
```

这条链同时证明了三件事：配置写入真的进了实例（否则还是默认的 2）；内核按实例自己的舍入模式
运算（RNE→1.0，RUP→1.0000001）；显示器的缓存被渲染路径与用例共用且每帧刷新。

引脚布局也在这里定死：本构建给两脚一侧的偏移是 `in0=(-2,0) in1=(-2,1)`、
`out0=(2,0) out1=(2,1)`，夹具与绘制都按实测值对齐（第一版夹具按 `-1/+1` 猜，显示器一直显示 0，
日志里的 `registered custom … in0=…` 直接给出了正确答案）。

### 同一用例的外观断言：本体、名字、值、引脚（2026-09-25 晚）

玩家看到的问题是"浮点元件的字太大、位置不对，引脚还压在本体上"。判据改成可测量的：**原版
Constant 的排版**——本体 4.92 × 2.93 格、名字右对齐在右上角（右缘距本体右缘 0.21 格）、
位宽框在左上角（内缩 0.17 格，1.00 × 0.66 格）、值 0.59 格高并居中、引脚在**本体外面 0.54 格**
（kind 表 `out0=(3,0)`）。参考数据来自玩家的原始截图（30.3 px/格：名字 11 px、位宽数字 12 px、
值 18 px、引脚中心距本体右缘 14 px）与 `asset/component_sprites/com_constant.png`。

用例现在会截图（`TC_MODLOADER_SHOT`），并让 Mod 逐帧上报每个实例的布局
（`TC_FLOATOPS_LAYOUT=1` → `float-ops render: type=… unit=… body=… pin0=… value="…"`），
于是它量的是**截图那一帧**的像素，而不是日志里第一次画出来的位置。断言：本体尺寸、名字行高度与
右上角落位、位宽徽章的框与原位、值的高度（被缩小的长值另判"放得下且可读"）、值是否居中、
引脚与本体边缘的距离。

真机结果（`tests/float-ops-playtest.ps1`）：

```text
Native logic: registered custom 0x463332434f4e5331 … out0=(3,0,w32)
Native logic: registered custom 0x4633324144445f31 in0=(-3,0,w32) in1=(-3,1,w32) out0=(3,0,w32) out1=(3,1,w5)
Native logic: registered custom 0x4633324449535031 in0=(-3,0,w32)
PASS float-ops render type=0x463332434f4e5331 unit=25.6px body=4.92x2.93 cells badge=0.82x0.51 at 0.23,0.23 name=0.51 cells inset 0.27 value=0.35 cells pin outside by 0.55 cells
PASS float-ops render type=0x4633324144445f31 unit=25.6px body=4.92x2.97 cells badge=0.86x0.51 at 0.23,0.23 name=0.55 cells inset 0.27 value=0.39 cells pin outside by 0.55 cells
PASS float-ops render type=0x4633324449535031 unit=25.6px body=4.92x2.97 cells badge=0.82x0.51 at 0.23,0.23 name=0.55 cells inset 0.31 value=0.59 cells pin outside by 0.55 cells
PASS float-ops M2 vertical slice: constant configuration -> FP32 Add (RNE then RUP) -> display text;
4.75 and the halfway case 1.0+2^-25 both reached the board
```

三个值的含义：Constant 那一行是长值 `2.9802322e-08`（13 字符，按本体宽度等比缩小到 0.35 格，
用例按"缩小的值"判；短值走 0.59 格的严格判据），Add 是中央的 `+`，Display 的 0.59 格就是它
十进制那一行（位模式下另有一行 0.40 格）。引脚外移 0.55 格与原版的 0.54 格相符；本体尺寸与
原版完全一致。**"引脚落到 ±3 之后电路还能跑"**由同一个用例的算术断言给出：内部走线跟着引脚走，
`4.75` 与半 ULP 用例照旧到达显示器。

字号标定（同一批真机测量）：本构建按请求字号的 **0.79** 倍绘制，画出的数字高约是那一版的
**0.66** 倍，所以"数字高 0.59 格"对应请求 `0.59 × 每格像素 ÷ 0.52`；名字与位宽数字按 0.40 格
反推。宿主日志 `Component render text: sized text armed, face=NoroshiCode_Regular.ttf size field=45.00`
记录了这一轮读到的字体与字号字段，两个常数都在 `examples/float-ops/components_internal.hpp`
的注释里标了出处。

**回归**：默认通道的既有类型未受影响——`tests/clock-period-playtest.ps1` 通过
（`PASS CLOCK-DRIVER: PASS the box opened the window, "8" + Enter set the period from 1 to 8 …`），
脚手架在 `pin_lane == 2.0` 时仍写历史坐标 `-18 / +13`，`tests/component-geometry.cpp` 的
`footprintReachesPins` 真假表加了"按类型通道"的三条。

### 生成电路的内部布局必须在引脚矩形内（2026-09-25 晚，玩家报"右下角多了一块"）

玩家截图里 FP32 Add 右下方多出一个方块，并自己给出了正确的解释："游戏识别到这个元件内部右下角
有什么元件然后自动绘制"。核对方式是把图里的坐标量出来：以引脚（±3 格，图中相距 150 px）定出
每格 25 px，则多出来的方块中心在元件中心右 **6.6 格**、下 **13.8 格** = 电路坐标
`(52.6, 110)`，与旧脚手架的依赖门 `(40 + 12·1, 100 + 8·1) = (52, 108)` 逐项相符。

**那张图是什么**：游戏为自定义元件渲染一张"内部电路"图（`asset/capture/com_custom_<id>.png`，
随包的示例可见其形状——内部逻辑若干个色块 + 两侧引脚点、透明底），元件栏、元件工坊和预览都用它。
我们的脚手架在那个位置放了依赖门，于是图上就多出一块。

**修法**：把脚手架自己的节点全部收进引脚矩形内——输入收集门 `x=-6`、依赖门 `x=0`（与输入行同高）、
输出驱动门 `x=6`，依赖链的接线随之重画（链走依赖门左一列、输入从收集门输出点接入、链尾接到第一个
驱动门输入），最终一版导线在 `y=0` 上走 `driver x-3 → driver x-1`。节点**种类与顺序**不变
（`0x4f…/0x12…/0x17…/0x12…/0x51…`），只有坐标变。

离线判据（`tests/native-component.cpp`，`tests/native.ps1` 跑）：

```text
PASS declarative API guards, 8x8 definition, invalid shapes, cross-word payloads, collector ordering,
emission phases and runtime constants
```

其中新增两条断言：两脚一侧（3.0 通道）与 16 脚形状（默认通道）的定义逐节点检查
`x ∈ [引脚列]`、`y ∈ [引脚行范围]`，并核对九种节点的顺序。

真机回归（脚手架是每个声明式元件的内部电路，所以这一改要跑全套）：

```text
PASS float-ops M2 vertical slice: … 4.75 and the halfway case 1.0+2^-25 both reached the board
PASS float-ops render type=0x4633324144445f31 … pin outside by 0.55 cells
PASS example.byte-adder: refresh and simulated cycles both compute carry_in + A + B
PASS declarative 8 inputs / 8 outputs: order checked, level reads outputs 6 and 7
PASS CLOCK-DRIVER: PASS the box opened the window, "8" + Enter set the period from 1 to 8 …
PASS text-box render clip pixels left=800..831 right=1768..1799 top=200..231 bottom=818..849
```

这一次**引脚位置没有变**（仍是 ±3 / 默认 ±2），改的是内部结构与那张内部图，所以既有存档里的连线
照常有效。

### 配置写入必须让暂停中的棋盘自己更新（2026-09-25 晚，玩家报"FP32DISP 更新不及时"）

玩家现象："我开始仿真刷新一下它才会更新"。先排除"回调没进运行态"：`TC_MODLOADER_DUMP_SOURCE=1`
留下生成源码（`native-logic-source.txt`），运行态程序里纯汇元件（FP32 Display，0 输出）的调用在

```text
// 18 com_not_word 1 1152921504606863363
var vid347 = U1 game_engine.'tc_logic_invoke'(U64 4, U64 (cycle + 1), U64 (vid328), U64 0)
```

刷新态里也有对应的 `tc_logic_peek(4, …)`；两边的回调次数普查（元件自己打的 `call census`）显示
三种元件的相位分布一致。所以**不是**"汇元件的回调被剪掉"，而是：**游戏的编译器不知道自定义元件的
配置变了**——玩家改完值，暂停中的板子仍然显示上一次算出来的结果，只有按刷新才会重算。这也解释了
为什么只有 Display 显眼：Constant 本体上的数字是渲染回调直接从它的配置画的，改完立刻变。

修法是**放回元件自己那一层**：它提交配置之后发出玩家那个刷新请求（`tc.simulation::pause` =
`sim.do` command 1，走同一条钩子链），棋盘随即带着新配置重算，线上标签与下游 Display 立刻跟上。
这一版先在加载器侧试过两种"自动刷新"，都被实测否掉：

1. 按"最后一条 `sim.do` 命令"判断是否在跑 —— 跑到目标自己停下的 run 不会再发命令，判断为"在跑"
   而跳过刷新；
2. 每帧读一次 `sim.cycle` 来判断（`simulationAdvancing`）——`text-box` 用例的相机平移检查随即
   失效（它看不到那四个实例了），说明从帧路径读游戏自己的计时是会扰动板子的；
3. 干脆无条件刷新 —— 同样打断 `text-box` 的平移。

留给元件自己发请求，既拿到"改完立刻生效"，又只影响发出请求的那个 Mod。

真机判据（`tests/float-ops-playtest.ps1`）：驱动写完全局配置后**不运行、不复位**，只等一帧再读
显示器：

```text
float-ops M2 test: right after the write, with no run, display shows "4.75 0x40980000"
PASS float-ops M2 vertical slice: … 4.75 and the halfway case 1.0+2^-25 both reached the board
```

日志里另有两条有界记录，便于排查"为什么这次没刷新"：

```text
float-ops: the board was refreshed after a configuration edit (status 0)
```

### 编辑器搬进游戏的底部元件面板（2026-09-25 晚，玩家要求"和原版一样"）

玩家原话："浮点元件点击后就会跳出自己写的界面，我觉得有点不好，应该把界面绘制在底部面板上……
原版的元件值、标签都是在面板完成更改的"。做法见 `docs/changelog.md` 的同日条目：加载器新增
`TC_UI_SLOT_BOARD_COMPONENT_PANEL`（钩 `build_toggle_button`，游戏排完自己的行之后调用选中元件
所有者 Mod 的回调，并把手上的实例 id 传给它），浮点族在面板里画 `标签` + `常量值`／`舍入模式`／
`显示方式`，标签写进配置（schema 3，空标签回落到类型名）。

真机证据：

- 加载器与 Mod 的注册行：`Component panel: the game's component drawer is hooked for Mod editor rows`、
  `UI slot registered: editor (component panel)`、`float-ops: the editor lives in the game's component drawer`；
- 选中一个 FP32 Add 之后：`Component panel: the drawer drew, selected=1 showing custom 0x4633324144445f31 …`
  与 `float-ops: the drawer is editing instance 0x1000000000000002 (type 0x4633324144445f31)`；
- 截图里的面板：游戏自己的标题（`FP32 Add`）与说明之下，多出 Mod 的两行
  `标签 元件类型名`、`舍入模式 nearest, ties to even (RNE)`，与元件本体之间**没有任何浮动窗口**；
- **面板里的行不接输入**（玩家实测"输入框和勾选框点了没反应"）：游戏那个窗口不把插件画进去的
  控件交给 ImGui 的输入路径。所以行改成摘要，点击由 Mod 自己命中测试（引擎的 `igGetMousePos` +
  `igIsMouseClicked_Bool`，与它原来打开编辑窗用的是同一对入口），点中哪一行就开哪一行的编辑窗：
  `float-ops: the panel row opened the label/value/rounding/display editor for 0x…`。输入框、
  单选框、Enter/确定/取消/Esc 都在那个窗口里生效——这也正是游戏自己的模型（元件数值行点开一个
  小编辑器）。
- 标签回读：驱动写 `left` 之后，渲染回调的布局行是 `… value="1" name="left"`。

自动化上的取舍：选中元件需要一次真实点击，而沙箱里的游戏窗口是隐藏启动的，合成按键只在游戏恰好
拿到指针时才生效（而且那时点到的可能是游戏自己的数值弹窗，把后面量像素的用例带偏）。所以
`tests/float-ops-playtest.ps1` 里硬断言的是"加载器已为 Mod 编辑器挂上元件抽屉"这一条，选中后的
两行日志与上面那张截图由人工跑一遍留证 —— 这一点在 `tests/test-catalog.json` 的用例说明里写明。

## 浮点元件族 M3 + M4：二进制32元件目录（2026-09-26）

### Mod 自己的元件栏（2026-09-26，同日追加）

玩家要求"把浮点数元件在右侧元件栏单独开一个文件夹"。实测那 5 个分类是游戏写死的、Mod 加不了页
（`docs/research/palette-categories.md`），所以按方案 1 交付了 **Mod 自己的"浮点元件"面板**：
加载器的电路板侧栏插槽列出 22 个元件，点一行就用命令总线把该元件放到棋盘上（游戏自己的放置通路，
Ctrl+Z 可撤销），落点会避开已有元件的占地。真机门禁：

```text
[local.float-ops] UI slot registered: palette (board side panel)
[local.float-ops] float-ops palette: the Mod's own component palette is a board side panel
[local.float-ops] float-ops palette: placing FP32 Multiply at (-4,9) status=0 request=1
[local.float-ops] float-ops palette: placement command state=3 result=0
[local.float-ops] float-ops palette: placement confirmed type=0x4633324d554c5f31 at (-4,5)
Board panel local.float-ops/palette frame=520 x=961 y=72 w=225 h=420 window=1200x800 content=967,97 size=213x388
PASS float-ops palette: the Mod's own palette listed its types, a real click on the FP32 Multiply row
     went through the loader's placement command, and the Mod confirmed the new instance on the board
```

（上面的 `placing` 与 `confirmed` 位置不同是因为命令先按净空搜索给了一个候选格，游戏自己的放置再
把它吸附到最近的合法格；两行都是实机输出。）面板按钮的真实鼠标点击由既有的
`tests/ui-board-panel-playtest.ps1` 覆盖——同一个侧栏容器里的按钮，用真实鼠标消息驱动。

判据是 `docs/PLAN-float-components.md` §6.2、§6.3、§7.2 与 §11：把 M3 基础包与 M4 扩展做完，
外观与已有三个元件同格式，有属性选择的元件在底部面板改。

**离线层**

```text
PASS float-ops binary32 kernel: 7571 vectors matched bit for bit (values and the five flags)
     across 12 operations and 5 rounding modes; canonical NaN, per-call flags and per-call
     rounding hold; SoftFloat state is per-thread (two threads, 200000 operations each)
PASS float-ops M0/M2/M3/M4: five public V2 probes plus the three M2 components and the
     catalogue; every catalogue type registers with its declared pins, the shared schema-3
     configuration and the 3.0 pin lane, and its callback publishes the kernel's result and flags
```

- `tests/float-kernel.cpp` 新增 `checkConversions`：把一个整数→FP32、FP32→整数的**策略**写死成
  用例（2^24+1 在 RNE/RUP 下的位模式与 NX；`INT32_MAX`、`UINT32_MAX` 的舍入结果；`-0` 转 0 无 flag；
  NaN/±Inf/越界置 NV 并饱和到 `INT32_MAX`/`INT32_MIN`/`UINT32_MAX`/`0`；负的非零值转无符号按越界；
  可精确表示的整数往返不变）。TestFloat 不覆盖这些项目决策，所以它们以项目自己的黄金用例固定。
- `tests/float-compat.cpp` 对 19 个新元件逐个断言：`input_count`/`output_count` 与表里一致、
  每条引脚的名字与位宽、`config_size == sizeof(OpsConfig)` 且 `config_schema == 3`、
  `pin_lane == 3.0`、callback 在给定输入下把内核结果与 flags 发到正确的引脚上
  （2−3、2×3、2÷3、√π、2×3+5、rem(5,3)=−1、round(π)=3、min/max、−1→−1.0、
  `0xFFFFFFFF`→2^32、NaN 的两种 int 结果、Classify 的 qNaN 位、Split→Make 逐位还原），
  外加 schema-2（无标签）迁移保持舍入模式。

**真机层**（`tests/float-catalogue-playtest.ps1`）

夹具 `tests/float-fixture.cpp --catalogue` 把 19 个新元件各放一个实例（外加 M2 的四个元件与三条
走线，让游戏把这盘棋编译出来），`dev.enter-board` 负责进关卡，驱动点元件本体打开游戏的元件抽屉：

```text
float-ops M3/M4: 19 more types registered (subtract, multiply, divide, square root, negate,
                 absolute, compare, classify, fma, remainder, round, min, max, i32/u32 both
                 ways, split/make bits)
float-ops render: type=0x4633324d554c5f31 unit=12.00 px/cell body=570,382..630,418 pin0=636,400 value="×" name="MUL"
…（19 个类型各一行，本体都是 60 px = 4.92 格宽；Compare 4.17 格高、Split 3.17 格高，其余 2.93 格）
DRIVER: the Mod's panel rows: label=96,709, rounding=96,766        ← 受舍入影响的类型
DRIVER: the Mod's panel rows: label=…, info=…                      ← Compare 的说明行
PASS float-ops catalogue look: 19 types drawn with the stock body width
PASS float-ops catalogue panel: a rounding type offered the five modes and the click wrote RUP
     back to that instance
```

（后面这条一开始只能断到弱版本——"提供了五种模式、没有选择的类型改而解释引脚"，因为这块棋盘有
23 个原生元件，而当时 Mod 的实例枚举固定 8 个 handle，**写入永远失败**；2026-09-26 修掉容量问题后
同一条用例直接断到"点击把 RUP 写回了那个实例"，见下面的"元件多到 8 个以上时配置写不进去"。）

同一天同一轮：`tests/float-panel-playtest.ps1`（面板输入的那条用例）重跑通过——
`PASS float-ops panel input: the rows in the game's own panel took the click, the typing and
the commit; the label reached the instance and the board, and no window opened over it`。
两个真机用例都会在游戏自己选错盘（启动时没打开夹具棋盘）时重试最多三次，并在日志里留下
`attempt N did not reach the end …; retrying`；这属于沙箱启动的抖动，不是 Mod 行为。

**仍未做**（明确留待后续，不属于本轮范围）：

- TestFloat 资格验证（M5，发布前用上游完整源码树单独跑）。
- binary16/binary64（M6，独立决策）。

### 面板里的输入框真的能用了（2026-09-26，玩家第二次反馈）

玩家第二次反馈（原话）："浮点数元件选中后底部面板的输入框，勾选框用不了"、"这个多出来的额外框我
不需要"。这一轮把它做成了面板内直接编辑，并且**删掉了弹出的编辑器窗口**：

- 根因（逐帧实测，见 `tests/float-panel-playtest.ps1` 的 trace 行）：鼠标停在插件画在抽屉里的
  那一行上时，游戏自己的抽屉窗口报 `panelHovered=0`——鼠标底下是抽屉内部的另一个子窗口，插件
  画在抽屉窗口本体里的 item 永远拿不到 hover，所以 `inputText`/`radioButton` 点不进去；
- 做法：三个元件的编辑行画在**Mod 自己的窗口**里（无背景、正好盖住面板那几行、每帧
  `setNextWindowFocus()`），字段是真的控件；`openEditor`/`drawEditorWindow`/确定/取消整个删除；
- 面板几何不猜：行窗口的位置由面板内容原点算出，行盒与字段盒写进
  `plugin-data/local.float-ops/panel-rows.txt` 供自动化点击。

真机门禁（`tests/float-panel-playtest.ps1`，隔离副本 + 独立 profile + 真鼠标/真键盘；棋盘是 M2
夹具，`dev.enter-board` 负责进入关卡，`test.float-panel` 驱动的点击与输入）：

```text
DRIVER: selected the constant component #1 id=…
DRIVER: the Mod's panel rows: label=96,709, value=96,766
DRIVER: hovering the label field at 289,734
DRIVER: clicked the label field
DRIVER: typed "xy" and pressed Enter
float-ops panel trace: frame=1127 … panelHovered=0 windowHovered=1 windowFocused=1
                        … r0=96,709..420,760 live=1 focus=1 label="xy"
float-ops: the label field was committed after editing
float-ops: instance 0x6383a201524e837d label "xy" (written)
float-ops render: type=0x463332434f4e5331 … name="xy"
PASS float-ops panel input: the rows in the game's own panel took the click, the typing
     and the commit; the label reached the instance and the board, and no window opened over it
```

用例同时断言"没有任何编辑窗口出现"（日志里不得出现 `editor window` / `###TCFloatOps`），这正是
玩家要的"不要多出来的框"。

回归：`tests/sandbox-sim-playtest.ps1`（大量写配置）、`tests/text-box-playtest.ps1`、
`tests/clock-period-playtest.ps1` 全部通过（clock 那次"点不开数值窗口"是它自己的点击计时抖动，
重跑即通过）。

同一条通知路径还包括**撤销/重做**：`undoConfigEdit`/`redoConfigEdit` 把配置改回去之后也会请求刷新，
否则撤销一次常量编辑会留下同样的陈旧输出（玩家给的例子：常量本体写着新值 `1`，它那根线上的标签
还是 `1078530010`，也就是旧值 `0x40490FDB` = 3.1415927）。`tests/component-undo-playtest.ps1`
仍然通过：`PASS one Ctrl+Z reverted one configuration commit and one Ctrl+Y re-applied it`。

### 多脚元件的占位与本体不重合（2026-09-26，玩家第五次反馈 + geometry V3）

玩家反馈："其它元件都好了这四个还是有点问题"（FMA 3 入 2 出、CMP 2 入 4 出、SPLIT 1 入 3 出、
MAKE 3 入 1 出）。从玩家截图量（1 格 ≈ 28.9 px）：四者的本体都**不在自己那格的中心**——游戏把生成的
出口脚从第 0 行往下排，所以 CMP 的本体覆盖 -0.33…3.33 格、SPLIT -0.6…2.6 格；入口脚按自己的数量
居中，于是 FMA/MAKE 的本体居中、CMP/SPLIT 的偏下。

两个可复现的后果：

1. `set_footprint(半宽, 半高)` 只能声明**居中**的框，宿主还把半高**向外取整**成整格：居中框要么在
   本体上方白留一格、要么漏掉本体下沿（矩形是游戏命中/选中/拖动/占位用的框，所以"可拖区域悬在本体
   上方空白处"也一并发生）。
2. 元件侧当时按"引脚跨度 + 0.6"长高本体，2.93 格的面被撑到 3.2（FMA/SPLIT/MAKE）与 4.17（CMP），
   整格占位因此变成 4 格与 5 格——比本体高 0.8 格，玩家看到的就是这四个之间、以及与别人之间的缝。

修法：

- **加载器**：`tc.component.geometry` 加 **V3**，保留 V1/V2 完整前缀，新增
  `set_footprint_cells(custom_id, x, y, width, height)`，按整格声明、**原样存储不取整**
  （`sdk/tc_service_api.h`、`sdk/tc_component_geometry.h`、`src/native.hpp`：`componentFootprintAllowed`
  共用三项校验，`writeComponentFootprint` 共用写入与通道告警，`setComponentFootprintCells` 新入口，
  `queryComponentGeometryService` 提供 V3）。单测 `tests/component-geometry.cpp` 加 V3 前缀
  static_assert 与带偏移矩形的 pack/unpack 往返；加载器全套离线测试通过；几何服务真机烟测
  `tests/component-geometry-playtest.ps1` 复跑通过（`set=0 packed=ok read=0 half=6.000000,3.000000`）。
- **元件侧**：本体只在两侧都塞不进原版 2.93 格面时才长高，且高度落在整格上；占位框取"本体覆盖的格 ∪
  引脚所在的格"，用 V3 按格声明（`examples/float-ops/components.cpp` 的
  `bodyRectForPins` / `footprintCellsFor`）。

真机门禁 `tests/float-pitch-playtest.ps1`（目录 id `float-pitch-game`）现在断言六个类型的间距：

```text
measure float-ops subtract   vertical pitch=3 cell(s)   (2 入 2 出，原版面 2.93 格)
measure float-ops constant   vertical pitch=3 cell(s)   (M2 参考)
measure float-ops fma        vertical pitch=3 cell(s)   ← 之前 4
measure float-ops split bits vertical pitch=3 cell(s)   ← 之前 4
measure float-ops make bits  vertical pitch=3 cell(s)   ← 之前 4
measure float-ops compare    vertical pitch=4 cell(s)   ← 之前 5；4 输出跨 0…3 行，最小可能值是 4
measure float-ops subtract   horizontal pitch=7 cell(s) (5 格本体 + 每根外伸脚 1 格)
measure float-ops constant   horizontal pitch=6 cell(s) (没有入口脚要躲)
```

（用例前身见 `tests/float-pitch-probe.cpp`：棋盘自己的放置命令逐格试探，每次测量独占一条 20 行的带，
被拒的尝试不留痕。）玩家截图里的四个元件现在：三个与其它元件同高（3 格），CMP 4 格，并且可拖区域与
画出来的本体精确重合。目录 look/panel、面板输入、元件栏文件夹、拥挤棋盘、玩家自己的存档棋盘五条真机
用例全部复跑通过。

### 垂直占位比本体大，元件贴不到一起（2026-09-26，玩家第四次反馈）

玩家反馈："这些元件在垂直方向上 footprint 好像有点大了，比外观要大一点，导致现在这些元件相互之间
不能贴在一起，而是中间有间隔。" 这一条**先量图再量真机**：

- 玩家截图按像素量：本体宽 129 px（= 4.92 格，1 格 ≈ 26.2 px）、高 76 px（= 2.9 格），
  但相邻两行的中心间距 103–110 px = **3.9–4.2 格** —— 占位确实比本体高约一格。
- 新探针 `tests/float-pitch-probe.cpp` 用**棋盘自己的放置命令**测最小间距：把参考件放在某格，
  再逐格（1…10）试放第二件，第一次被接受的那个偏移就是答案；每次测量独占一条 20 行的带、
  被拒的尝试不留东西，所以互不干扰。用例 `tests/float-pitch-playtest.ps1` 把结果写成硬断言：

  ```text
  measure float-ops subtract vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops subtract horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops compare  vertical pitch=5 cell(s)  ADJACENT-OK
  measure float-ops compare  horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops constant vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops constant horizontal pitch=6 cell(s)  ADJACENT-OK
  ```

- 根因：宿主把声明的半高**向外取整**成整格（`src/component_geometry.hpp` 的
  `fromHalfExtents`：`height = ceil(2 * halfHeight)`）。三个 M2 元件声明固定的 1.5 → 3 格；
  目录元件声明的是"本体半高 + 0.1"：2.93 格的面 → 1.465 + 0.1 = 1.565 → `ceil(3.13)` = **4 格**。
  同一张面，目录元件比 M2 多占一格，于是每对之间都留出一条可见的带（修复前实测 subtract 垂直 4 格，
  M2 constant 已经是 3 格）。
- 修法：目录元件改声明本体自己的半高（不再加 0.1），并与 1.5 的下限取大。2.93 格的面 → 3 格
  （与 M2 及游戏自带同尺寸元件一致）；3.17 格的 Split Bits 仍 4 格；4.17 格的 Compare 仍 5 格。
- 水平方向保持不变：5 格本体 + 每根伸到框外的脚 1 格（两侧有脚 7、只有输出脚 6）——
  脚在游戏自己的占位规则里也算占位（修复前后都是这个数，玩家没有反馈水平方向）。

回归：`-Fixture crowd`（22 元件棋盘）、目录 look/panel、面板输入、元件栏文件夹四条真机用例全部通过。

### 元件多到 8 个以上时配置写不进去（2026-09-26，玩家第三次反馈）

玩家反馈："现在所有能写配置的元件都写入不了，都是写入元件配置失败"（截图是底部抽屉的
"显示方式"一行 + 灰字状态）。这一条不是分类改动引起的，是**实例枚举的容量**问题：

`tc.component.instances` 的枚举是**全有或全无**：宿主只在"全部匹配实例都装进调用方缓冲区"时返回
`TC_COMPONENT_INSTANCES_OK`，否则 `TC_COMPONENT_INSTANCES_ERR_RANGE`（`src/native_logic.hpp` 的
`instanceEnumerate`：`return count==all?OK:ERR_RANGE;`）。本 Mod 用固定 8 个 handle 的栈数组，
于是棋盘上原生元件超过 8 个之后，枚举永远 `ERR_RANGE`，Mod 当成"这个实例不在棋盘上"→
**每一次**写入都失败。玩家棋盘 4 → 22 个元件之后必挂，而沙箱夹具（4 个实例）一直掩盖它。
另一个实测细节：实例集合会在游戏绑定编译结果的过程中继续变大，所以"按第一次的总数再枚举一遍"
仍可能 `ERR_RANGE`，必须循环重试。

修法：`instanceHandles()`（`examples/float-ops/components.cpp`）从 16 个起步，`ERR_RANGE` 时按宿主
报的总数 +1 扩容重试最多 5 次；`writeConfig` / `currentConfig` 都走它，失败时把原因写进日志
（`no live handle for instance …`、`this write was not attempted because the host reports …`、
`the host refused the configuration for instance …: …`）。

**新增夹具与用例**：`tests/float-fixture.cpp --crowd` 写 18 个不接线的 FP32 Display，**之后**才是
M2 链（常量/加法/显示），共 22 个原生元件——被编辑的常量排在很后面，正好在旧的固定 8 个 handle
之外。用例 `tests/float-panel-playtest.ps1 -Fixture crowd`（目录 id `float-busy-board-game`）跑的是
与面板用例一样的点击/输入/提交，并额外断言日志里不出现上面三条失败诊断：

```text
PASS float fixture (crowd): components=22 bytes=1832 …
PASS float-ops panel input (crowd board): the rows in the game's own panel took the click, the
     typing and the commit; the label reached the instance and the board, and no window opened over it
```

**这条用例真的能抓住它**（做过反向验证）：把 `instanceHandles` 改回"固定 8 个 handle、不重试"，
同一条用例失败在 `Missing evidence: label "xy" (written)`；改回修复版后通过。

**玩家自己的棋盘**（22 个原生元件，`-Board <玩家存档>`）也用它跑过：修复前日志是
`float-ops: the host would not list its instances, so this write was not attempted` +
`instance 0x… label "xy" (refused)`，修复后同一条命令 PASS，标签真的写进实例并显示在棋盘上。

### 浮点文件夹是游戏自己建的（2026-09-26，路线 2）

玩家否掉了 Mod 自己画的元件栏（上一节的路线 1）："我希望分类文件夹是真的插进游戏自己的分类体系。"
这一轮不再自己画元件栏，而是让**游戏自己**建文件夹。机制是实测出来的，不是猜的：

- 树在哪：`get_component_menu__presenterZutilities_u9607` 建树 →
  `reload_component_menu__presenterZutilities_u14192` 把它写进 presenter context 的
  `+0xd948`（数量）/`+0xd950`（指针数组）；
- 节点长什么样（0x30 字节）：`+0x00` 节点编号、`+0x01` 变体标记（`&7`：2 = 分类、1 = 自定义元件、
  0 = 内置元件）、`+0x08` 名字（UTF-8 Nim 字符串）、`+0x20/+0x28` 子节点数量/数组，
  **第 i 个子节点在 payload + 8 + 8*i**；
- 谁分文件夹：`add_to_menu_tree__presenterZutilities_u12208` 把**自定义原型的名字按 `/` 切开**，
  逐段当父分类走进树，根节点是游戏自己建的"自定义"。

所以交付是一行名字前缀：元件注册成 `浮点/FP32 Add`，游戏就把它们收进自己建的 `浮点` 分类节点
（`examples/float-ops/components.cpp` 的 `namePrefix`，`TC_FLOATOPS_PALETTE` 可覆盖）。Mod 不写
游戏内存，不写玩家存档；上一轮的 `registerPalette`/`paletteDraw`/`placeType`/`freeSpot` 与
"点一下放到棋盘"整条通路已经删除。

**真机门禁** `tests/float-palette-playtest.ps1`：探针 `tests/float-menu-probe.cpp` 钩住游戏自己的
`reload_component_menu__presenterZutilities_u14192`，等它建完菜单后把整棵树 dump 到
`build/menu.txt`（每个节点一行：变体标记、名字、自定义 id），用例断言的是**游戏这棵树**，
不是 Mod 自己报的东西：

```text
node=… byte0=151 byte1=2 tag=2 name="自定义"
  children=1 payload=…
  node=… byte0=171 byte1=2 tag=2 name="浮点"
    children=23 payload=…
    node=… byte0=171 byte1=2 tag=2 name="M0"
      children=5 payload=…
      node=… byte0=0 byte1=1 tag=1 id=0x4633325041535331 name="浮点/M0/FP32 M0 Pass"
      …（五个 M0 兼容性探针）
    node=… byte0=0 byte1=1 tag=1 id=0x4633324144445f31 name="浮点/FP32 Add"
    …（22 个可用元件的 id）
PASS float-ops palette: the game built a 浮点 folder inside its own 自定义 category and filed
     all 27 float components (22 types + 5 M0 probes) under it; none of them is listed anywhere
     else in the game's palette tree
```

用例还断言游戏原有的五个分类都还在（`tag=2 name="布尔"` … `name="自定义"`），避免"删了别人的
分类换来自己的文件夹"。dump 文件留在 `build/menu.txt`，探针只读（每次解引用前先 `VirtualQuery`）。

**已知边界**（`docs/research/palette-categories.md` 记了可行步骤）：文件夹在"自定义"**里面**，
不是与布尔/整型/杂项/输入输出并列的顶级页——钉住的这版游戏里，自定义原型的菜单路径固定从
游戏自己的"自定义"节点起步。要做成顶级页只能钩完 `reload_component_menu` 之后改游戏自己的菜单
树（分配节点、把节点从 `自定义` 的子序列搬到顶级序列），那是在玩家进程里改 Nim 对象/序列，
本轮没做。

## 浮点元件族 M1 数值内核（2026-09-25）

M1 的判据是 `docs/PLAN-float-components.md` §5 与 §10.1：内核是 vendored SoftFloat 3e，公开语义
由本项目固定，离线数值层必须由独立 oracle 出题。

**语料怎么来的**：`tools/float-vectors.py` 用 Python 的精确有理数与整数运算独立实现 binary32 的
舍入（五种模式）、tininess-after-rounding、OF/UF/NX、canonical NaN、quiet compare、十位分类、
2019 min/max 与 IEEE remainder，生成 `tests/data/float32/fp32-vectors.generated.hpp`。
生成前脚本先自检，任一条不过就不写文件：

```text
wrote tests\data\float32\fp32-vectors.generated.hpp: 7571 vectors,
960 cross-checked against double arithmetic
```

- 960 条最近舍入向量与 CPython double 算术“先 double、再一次舍入到 binary32”逐位一致（对
  `+ - × ÷ √` 成立：double 的位数超过 `2p+2`，所以二次舍入无害）；
- 每条结果还要过一个按定义写的性质检查：定向模式必须落在定义要求的那一侧、其相邻值必须跨过精确值，
  最近模式必须在半个 ulp 以内，开方用平方比较（避免出现无理数）。

这两层检查在开发中真的抓到了两个 **oracle 自身**的错误（RDN/RUP 方向写反、remainder 的符号处理），
所以内核不是拿自己生成的结果当答案。

**fast 门禁**：`tests/float-kernel.cpp`（`examples/float-ops/build.ps1` 编译并执行）。它读上面的
语料，只比较，不自己算期望值：

```text
float-ops binary32 kernel: 7571 golden vectors
  add 1220 / subtract 1220 / multiply 1220 / divide 1220 / square_root 860
  fused_multiply_add 715 / remainder 244 / round_to_integral_exact 350
  compare 110 / classify 172 / minimum_number 120 / maximum_number 120
PASS float-ops binary32 kernel: 7571 vectors matched bit for bit (values and the five
flags) across 12 operations and 5 rounding modes; canonical NaN, per-call flags and
per-call rounding hold; SoftFloat state is per-thread (two threads, 200000 operations each)
```

除语料之外，这个测试还固定了三条契约：算术结果一律 canonical `0x7FC00000`（qNaN 不置 NV、sNaN 置
NV），位操作保留 payload；flags 只属于本次调用（`1.0/0.0` 之后 `1.0+1.0` 必须是 0）；舍入模式
只属于本次调用。最后一条用一个双线程用例兜底：两个线程各做 20 万次不同舍入模式的运算，任一次结果
不对就失败——这条用例只有在 `THREAD_LOCAL` 真的生效时才会通过。

**真机层**：内核编进 `local.float-ops.mod` 的 DLL，插件在加载时跑一次自检并把结果写进日志
（`tests/float-compat-playtest.ps1` 与 `tests/float-boundary-playtest.ps1` 都断言这一行）：

```text
[local.float-ops] float-ops M1: kernel self-check 1+2=0x40400000/0x00
(1+ulp)^2=0x3F800002/0x01 fma=0x34000001/0x00 1/0=0x7F800000/0x08
sqrt(-1)=0x7FC00000/0x10
```

其中 FMA 一条是特意选的：先乘后加会给出 `0x34000000`，fused 给出 `0x34000001`，所以这一行同时
证明“真的在做单次舍入的 FMA”。真机 DLL 里 `softfloat_roundingMode` 是 TLS 符号
（`nm` 可见 `_ZTH22softfloat_roundingMode`），即 M0 记录的那份 DLL 里带的就是每线程状态。

**发布候选仍未做**：§10.1 要求的 TestFloat 资格验证要到 M5 产品化时跑（TestFloat 用上游完整源码树
单独构建，不链接本 Mod 的 19 个编译单元）；十进制 parse/format 的 vendored `fast_float`/Ryu 属于
M2。

## 浮点元件族 M0 兼容性（2026-09-25）

M0 的判据是 `docs/PLAN-float-components.md` §4.4：先证明当前 Loader 能可靠承载这个独立 Mod，
再谈数值内核。所有用例都在隔离副本 + 独立存档里跑，生产包 `dist/local.float-ops.mod` 里没有
任何测试用关卡驱动。

**离线层**（`examples/float-ops/build.ps1` → `tests/float-compat.cpp`）：五个公共 V2 形状
（0 入 1 出、32 位直通、`R[32]+Flags[5]` 双输出、`Flags[5]` 纯汇、32 位纯汇）注册成功，
`0x00000000`、`0x00000001`、`0x7F7FFFFF`、`0x7F800000`、`0x7FC00000`、`0x80000000`、
`0xFFFFFFFF`、`0xDEADBEEF` 八组位模式在 `RESET`/`REFRESH`/`CYCLE` 三种相位下逐位往返；同一次
测试断言 `tc.component.types` 缺失时插件必须加载失败。

```text
PASS float-ops M0: five public V2 shapes registered; eight FP32 bit patterns survived
RESET/REFRESH/CYCLE; R[32]+Flags[5], a Flags[5] sink and the pure source/sink are stateless
```

**包层**（`tests/float-package.ps1`）：隔离沙盒里 `list` 发现包、`apply` 启用、`disable-all`
停用，只写 `mods/` 与 `tc-modloader-data/`，`native` 摘要被记录也能被清空，没有写 Loader 或
游戏二进制。

```text
PASS float-ops package: discovered, enabled and disabled in isolation; no Loader/game binary was written
```

**真机层**（`tests/float-compat-playtest.ps1`）：夹具 21 个实例，
`source → pass ×17 → result+flags → sink`，另把第二输出 `Flags[5]` 引到自己的五位汇。实际绑定
token 1–21，四个 32 位形状逐实例读到完整 `0xDEADBEEF`，`Flags[5]` 被读回 `0x0000001f`。测试
专用驱动在板子编译完成后接管仿真，显式走「跑 6 周期 → 暂停 2.5 秒 → RESET → 再跑 6 周期」：

```text
float-compat: board compiled, driver will take the simulation over
float-compat: driver took over the simulation
float-compat: run issued target=6
float-compat: paused after cycle=0
float-compat: pause window stable at cycle=0        <- 暂停窗口里仿真没有前进
float-ops M0: source refresh observed, instance=0x1000000000000000 cycle=0 value=0xdeadbeef
float-ops M0: pass refresh observed, instance=0x1000000000000001 cycle=0 value=0xdeadbeef
float-ops M0: result-flags refresh observed, instance=0x1000000000000012 cycle=0 value=0xdeadbeef
float-ops M0: sink refresh observed, instance=0x1000000000000013 cycle=0 value=0xdeadbeef
float-ops M0: flags-sink refresh observed, instance=0x1000000000000014 cycle=0 value=0x0000001f
float-compat: reset issued
float-compat: resumed after reset, target=6
float-compat: post-reset cycles reached cycle=0      <- 复位后周期线仍旧带完整位模式
PASS float-ops true-game runtime: 21 instances bound (tokens 1..21), 0xDEADBEEF reached
source/pass/result+flags/sink callbacks, the Flags[5] output reached its own sink, refresh
rendered the values while paused, and the post-reset cycles still carried them
```

每次 RESET 的汇总行还要报告它结束的那一段：`previous cycle=… refresh=… mismatched=0`，非零
`mismatched` 会让用例失败，所以“探针读到的值”不是靠肉眼比对。

**token 边界诊断**（`tests/float-boundary-playtest.ps1`，报告 `build/float-boundary-report.txt`）：
同一条链加长到 42 个实例（`pass ×38`）。token 1–33 读到完整位模式，token 34 起读到
`0x00000000`：token 33 是第一个没有自己的状态槽、因而没有发布宽输出的 token
（`src/native_logic.hpp` 的 `token > kBridgeStateTokens` 直接跳过发布）。顶点是确定性的空值，
读取仍落在仿真状态缓冲区内，没有越界写入。这就是 §4.2 记的 32 token 上限：**当前 Loader 下
整板原生 binding token 应保持在 32 以内**。

```text
   31 0x100000000000001e  pass         0xdeadbeef  0xdeadbeef
   32 0x100000000000001f  pass         0xdeadbeef  0xdeadbeef
   33 0x1000000000000020  pass         0xdeadbeef  0xdeadbeef
   34 0x1000000000000021  pass         0x00000000  0xdeadbeef   <-- diverges
  ...
   42 0x1000000000000029  flags-sink   0x00000000  0x0000001f   <-- diverges
result: the first probe that read a wrong value was token 34; its input comes from token 33,
so token 33 is the first wide output the loader did not publish
PASS float-ops boundary diagnostic: tokens 1..32 hold 0xDEADBEEF; token 33 is the first wide
output without a slot (its consumer, token 34, read 0 instead)
```

## 文本框节点缩放（2026-09-23）

### 节点独占鼠标：拖动缩放节点不再同时放线（2026-09-23 起）

用户报告的症状是"拖文本框节点时会同时触发导线放置"。真机复现与定位过程如下，结论是
**"回答有控件在活动"不是棋盘的输入闸门**：

1. 复现：自动用例用真实鼠标事件在节点上按下、拖 2 格、松开（同一场景 `tests/text-box-playtest.ps1`）。
   自带的看门狗报出棋盘对象在松开那一帧变化，并把那条线的两端点打出来：

   ```text
   text-box autotest: board objects changed components=8->8 wires=0->1 stage=83 block-requested=1 resizing=1
   text-box autotest: wire inventory [0] -3,-2->-5,-3 width=1
   ```

   两个端点正是这次手势的按下点与松开点（板面坐标），所以它不是关卡自带、也不是别的阶段的产物。
2. 当时实现只在 `build_board_ui` 的采样点（返回 RVA `0x46b593`）把 `igIsAnyItemActive` 答成 true。
   给该入口挂钩子并统计：整个手势期间棋盘 **76 次**采样全部被这一层接住（`blocked`），
   导线照旧出现 —— 单靠这一层不成立。
3. 反汇编（`build/exe-disasm.txt`）解释了为什么：该采样点只决定 `handle_io_on_board` 走哪条分支；
   光标落在棋盘窗口内时走的那条分支依然会调用 `handle_ongoing_action`，而手势的提交就在那里。
4. 真正有效的闸门在 `handle_io_on_board` 开头：它每帧一次性读四个鼠标按键状态
   （`0x1403558c7`..`0x140355935`，`igIsMouseDown_ID`／`igIsMouseClicked_InputFlags`／
   `igIsMouseDoubleClicked_ID`／`igIsMouseReleased_ID`）并打包进自己的 io 状态，之后整个棋盘都用
   这份拷贝。文本框在节点被悬停或正在拖动时，只对这些返回地址改写答案：**棋盘读到的是"没按下"**。

默认门禁（跑一次即可复现全部数字）：

```text
PASS text-box resize handles width=7.000000->10.957347 height=3.600000->5.596587 board-position=unchanged
PASS text-box text layout follows resized top edge
PASS text-box resize handles captured board input samples=76 board-objects=unchanged mouse-reads-hidden=608
PASS text-box hidden note drag from -6,-18 to -5,-18 selection=1 instance=…
```

前三条一起说明：尺寸确实变了（手势真的被 Mod 拿到）、元件坐标没动、棋盘的采样被接住且棋盘自己的
鼠标读取被替换了 608 次；而**整个手势前后棋盘元件数与导线数没有变化**（`components=8 wires=0`）。
最后一条是同一轮里的正对照：普通元件拖动照旧能移动便签，所以"没动"不是棋盘失灵。

灵敏度对照（同一段代码、同一串合成事件，只把独占关掉）：

```powershell
$env:TC_TEXTBOX_BLOCK_BOARD_INPUT='0'; ./tests/text-box-playtest.ps1
```

```text
text-box: resize input block is OFF (control run)
text-box autotest: resize step press    components=8 wires=0 samples=15 blocked=0 leaked=15
text-box autotest: resize step release  components=8 wires=1 samples=49 blocked=0 leaked=49
PASS text-box resize control: without the block the same drag reached the board
```

即关掉独占后导线立刻回来（`wires=0->1`，在 83 阶段提交），说明默认运行里的 0 是量出来的，不是
窗口本来就没有东西。脚本按 `TC_TEXTBOX_BLOCK_BOARD_INPUT` 自动切换要求哪一组断言（两组互斥）。

边界与代价：独占只在**选中**的文本框的节点上成立（未选中时节点不画也不接管），鼠标移出窗口导致
按键状态没清的情况由"按住的元件已不在棋盘上就丢弃手势"兜底；四个钩子只认那一个返回地址窗口，
其余调用者（面板、菜单、ImGui 自己）拿到真实答案。

### 缩放本体

- `tests/text-box-playtest.ps1` 在真实游戏里选中便签，移动到右下角节点并用真实鼠标按下、拖动、松开；
  用例同时读取配置和棋盘模型，确认只改变可见框尺寸，没有偷偷移动元件：

  ```text
  PASS text-box resize handles width=7.000000->10.957347 height=3.600000->5.596587 board-position=unchanged
  PASS text-box text layout follows resized top edge
  PASS text-box native instance footprint selected outside type footprint by=1.13 cells
  ```

- 同一轮完整真机门禁继续通过隐藏便签拖动、选中提示、默认绘制抑制、四边裁剪和编辑器读回，说明缩放
  手势没有抢占普通元件拖动。扩展区域由 geometry V2 接入游戏原生点查询，所以选择、拖动与删除不再
  依赖 Mod 的虚拟状态；类型级 8×4 矩形只继续承担占位/放置。配置 v1→v2 的兼容由插件迁移回调负责。

## M5 geometry 第一刀（2026-09-22）

- 离线：`tests/component-geometry.cpp` 断言半宽边界、向外量化、`(-6,-3,12,6)` 的 64 位打包回读、
  90° 旋转宽高交换，以及 Geometry V2 完整保留 V1 ABI 前缀；`tools/abi.ps1` 固定 V1/V2 表布局与错误码。
- 真机研究：`TC_APPEARANCE_GEOMETRY=1 tests/component-appearance-playtest.ps1` 用游戏自己的序列
  析构/分配和 Prototype setter 把形状替换为 `(-6,-3,12,6)`，游戏完成加载、放置并保持稳定。
- 正式服务烟测：`tests/component-geometry-playtest.ps1` 注册一个真实类型，经 `query_service` 调用
  `set_footprint(6,3)`，从游戏当前 Prototype 反读后再经命令总线放到真实棋盘，最后用实例句柄调用
  `read_footprint`；结果为
  `PASS component geometry service set=0 packed=ok read=0 half=6.000000,3.000000`。
- 尚未计入通过：旧 hitscan 会保留梯子并污染后续偏移；精确放置边界与悬停是下一条独立真机用例，
  所以当前结论是“M5 geometry 第一刀完成”，不是“M5 完成”。

## 选中提示跟随 footprint（2026-09-23）

结论：**游戏自己的选中提示没法跟着 footprint，但 Mod 自己画的那一层可以，而且已经做到。**

- 游戏那条提示的机制（静态跟读 + 真机）：`focus_components__presenterZupdate95state95common_u5084`
  @0x140473700 并不画新几何，而是给**元件自己的网格**换颜色
  （`set_color_shared__...component95mesh_u3441` / 结尾的 `...component95custom95mesh_u1325`）；
  自定义元件的网格由 `get_mesh_custom__...component95mesh95factory_u2020`
  @0x140287830 从工厂的 `+0x228/+0x22c` 现建（`create__...component95custom95mesh_u405`），
  与 `tc.component.geometry` 写进原型的那条矩形序列无关——所以它的尺寸永远跟元件网格，不跟 footprint。
- 真机量化（真 0 脚版本）：拖走并保持选中后，白弧外接框约 **3.9 × 3.2 板面格**，而该类型的 footprint 是
  **8 × 4**；未选中的同类型实例整块空白（`build/text-box-out/hint-dragged.png`、`hint-measured.png`）。
- 改善：`examples/text-box` 现在自己读游戏的选中集（`sdk/tc_board_model.h` 的
  `tc::TCBoardModel`，键是**元件序列索引**，因此 `NoteOnBoard` 增加了 `index` 字段），在 render 回调里
  对选中的实例按声明 footprint（±4/±2 板面格）画橙色高亮环 + 淡填充。报告行给出中心、半宽高、
  屏幕外接框和当帧"一格多少像素"：

  ```text
  PASS text-box selection hint instance=2746930921361689319 centre=1261.0,293.1 half=4.00,2.00 box=1148.4,236.8..1373.7,349.4 unit=28.160 rotation=0
  PASS text-box selection hint follows footprint width=225.3px height=112.6px unit=28.16 rotation=0
  ```

  脚本对"外接框 == 8×4 格 ± 容差"和"外接框中心 == 元件中心"两条都做硬断言（含 90°/270° 时按 4×8 检查）。
- 像素回读：`TC_TEXTBOX_KEEP_SELECTION=1 tests/text-box-playtest.ps1` 保留拖动后的选中状态，
  进程内 BMP 用 `build/text-box-out/hint-pixels.ps1` 数橙色像素，实测
  `orange hint pixels=1347 bbox=1176,236..1402,350`，即 226.6 × 114.6 px @ 28.16 px/格 = **8 × 4 板面格**
  加上 2.5 px 线宽；证据图 `build/text-box-out/hint-selected-box.png`（橙色框 + 里面游戏的白色弧线）。
- 顺带修掉一处用例抖动：编辑器刚关闭就发拖动按下，会被关窗那几帧吞掉（实测一次
  `-6,-18 -> -6,-18` 未移动）；现在先 `endEditing()`，下一帧再按。

### 游戏自己那圈白弧已按类型关掉（2026-09-23 第二轮）

结论：**那圈白弧可以关，而且已经关掉并逐像素验收。** 上面的"未做"一条作废，原因见下面第一条：
真正的绘制 pass 不是 `focus_components`。

- **真正的 pass**：`redraw_selection__presenterZupdate95state95common_u5104` @0x140477480，整个二进制
  里**只有一个调用者** `update_state_move_selection__presenterZupdate95state95dynamic_u69`。
  `focus_components__...u5084` 是死靶：棋盘阶段一次都不被调用（挂上去只会让日志谎报"已接管"），
  加载器里的那个钩子已删除。它开头 `clear_all_instance(selection mesh)`，随后遍历 presenter 选中容器
  里的两个哈希集：`+0x00` 是 **selected_components**、`+0x18` 是 selected_wires；桶宽 0x20，
  桶 `+0x08` 是占位字（循环用它判空）、`+0x10` 是元素 id（元件这一侧就是**元件序列索引**）、
  `+0x18` 是圆环缩放。占位字非 0 就为这个元素加一个 selection 精灵实例
  （`init_transform_2d(桶索引)` + `toPacked_transform_2d_same_scale_no_rotation` 落在固定缩放上），
  `SameScaleNoRotation` 正是它不可能变成长方形的原因——所以正确路线只能是"游戏那圈关掉、Mod 按
  footprint 画自己的"。
- **归属判据用游戏自己的函数**：桶里的 id 被 `contains(selected_components, id)` 判真、
  `contains(selected_wires, id)` 判假（`tc_board_model.h` 用的同一对入口），实测确认索引空间就是元件
  序列，与宿主在 `dispatchComponentRender()` 里走的序列一致。于是宿主只把**已登记
  `set_selection_hint(id,false)` 的元件**那一桶的占位字清 0，原函数返回后立刻还原：该元件不产生
  精灵实例（不是"把变换改小"，是根本不画），其它元件、导线与内置元件的环完全不受影响。
- **真机证据**（同一条件、同一量法：取日志最后一次 `PASS text-box selection hint ... box=` 的框，
  内缩 18 px，数白像素；两次运行测的是**同一个实例的同一个窗口**，且截图那一帧它仍然被选中）：

  ```text
  抑制生效   PASS text-box game arc window=0 pixels instance=3243828408617115861 selected_at_capture=1 arcs_kept=0
  灵敏度对照 PASS text-box game arc window=546 pixels instance=3223796462552968311 selected_at_capture=1 arcs_kept=1
  日志       Component render: cleared the game's selection ring for 1 Mod component(s)
  ```

  第一行 `TC_TEXTBOX_KEEP_SELECTION=1`：被抑制的便签窗口 0；第二行
  `TC_TEXTBOX_KEEP_SELECTION=1 TC_TEXTBOX_KEEP_ARCS=1`（Mod 不调 `set_selection_hint`，把游戏那圈留给
  便签）：同一个窗口 546。两行的"最后一帧仍选中"由脚本断言（`selected_at_capture=1`），所以 0 是
  "关掉了"而不是"什么都没选中"或"窗口里本来就没有像素"。**作用域**（只关自己的类型）由两件事保证：
  宿主只清已登记类型的那个桶，且桶里那个 id 必须被游戏自己的 `contains(selected_components, id)` 判真、
  `contains(selected_wires, id)` 判假。想在同一帧里同时看到一个被抑制和一个未被抑制的元件环做不到：
  游戏里再按一个元件是**替换**选中（Shift-click 与框选在本棋盘状态下都不加选），脚本因此改用
  "同一窗口 + `KEEP_ARCS` 灵敏度对照"这一对来闭合。
- 回归：`tests/text-box-playtest.ps1` 默认运行仍是全绿，并新增两条硬断言：加载器日志必须出现
  `Component render: selection hint control armed` 与
  `Component render: cleared the game's selection ring for N Mod component(s)`；弧线场景（上面第一行）
  在脚本内直接量两个窗口，`≤4` / `≥100` 都写成了断言。
- 部署与字节可核对：`build-text-box.ps1` 现在和 `build.ps1` 一样设置 `SOURCE_DATE_EPOCH`，同一个源码
  连编两次得到**同一个** `.mod` 哈希（实测 `9DFAECC5…`）；部署后的
  `D:\p\game_engine.dll`、`D:\p\mods\local.text-box.mod` 与 `dist\` 里被沙箱跑过的两份逐字节相同，
  所以"跑过的就是装上的"可以用 `Get-FileHash` 复核。

### 模仿原版那圈提示：测过、验证过，**不投入使用**（2026-09-23 第三轮）

结论：**原版那圈可以照抄，而且抄得准；但按决定不做进示例**，参数与证据留档在
[reference/unused-interfaces.md](reference/unused-interfaces.md) 第 1 条。下面是这一轮量到的事实，
留在这里是因为它们是"原版那圈到底长什么样"的唯一实测记录。

- **原版几何实测**（同一个便签、两个相机缩放，`TC_TEXTBOX_ARC_ZOOM_OUT=3` 做第二次）：

  | 属性 | unit=28.16 | unit=21.157 | 板面单位 |
  |---|---|---|---|
  | 环心 | 元件原点 +(0.99,−0.02) 格 | +(0.97,−0.01) 格 | 局部 **+1.00 格（x）** |
  | 半径（射线首次命中 = 内缘） | 22–23 px | 17 px | **内缘 0.80 格** |
  | 线宽 | 6 px | 4 px | **0.21 格**（中心线半径 0.91 格） |
  | 白像素 | 540 | 304 | 面积比 = 缩放比² → **精灵随板面缩放，不是固定像素** |
  | 颜色 | 253,253,253 | 同 | 近纯白 |

  角度（+x 为 0°、+y 向下）：两段弧 **44°–137°** 与 **223°–315°**，左右各留约 86° 缺口。
- **原型做法**（已从示例撤出，见 [reference/unused-interfaces.md](reference/unused-interfaces.md)）：
  选中分支里按局部坐标采样两个弧段连折线，线宽 = `0.21 * unit`、圆心 = 局部 `(+1.00, 0)`、半径 = 0.91 格；
  这三个数字做成开关后，`半径 2.4 格 + 圆心居中` 就是"大圈套在元件正中"的那种效果。
- **同帧对拍**（比只看像素数可信得多）：让游戏自己那圈留着
  （`TC_TEXTBOX_KEEP_ARCS=1`），把模仿环画成橙色（`TC_TEXTBOX_HINT_ORANGE=1`）叠在同一个实例上，
  两者应当重合。实测：

  ```text
  orange hint pixels=443 bbox=1298,265..1337,321     ← 模仿环
  white arc  pixels= 84 bbox=1295,264..1338,321     ← 原版环没被盖住的部分
  原版单独跑：white pixels=540 bbox=1297,264..1337,321
  ```

  外接框四个方向都在 1–2 px 内，剩下的 84 个白像素是抗锯齿边（约 0.5 px 一圈），不是几何偏差。
  证据图 `build/text-box-out/hint-imitation-overlay.png`。
- **形状验证**（原型里做过的脚本断言，撤出示例后不再随门禁运行）：环上 10 条射线必须命中、缺口 6 条
  射线必须落空，实测 `arcs=10/10 gap=0/6 rays`。三张成品图：`hint-imitation.png`（只用环，抑制生效）、
  `hint-imitation-both.png`（环 + footprint 框）、`hint-imitation-custom.png`（半径 2.4 格、圆心居中）。
- 诊断开关 `TC_SELECTION_TRACE=1`：把两个选中集的桶、桶里每个 64 位字以及它在
  `selected_components`/`selected_wires` 里的归属、selection 精灵的生产者与被 `toPacked...` 打包的
  变换一起打进日志——上面那套 layout 就是这样定下来的，不要再靠"看不出来就猜"。

### 便签的默认尺寸与"随相机缩放"（2026-09-23 第四轮）

用户报的两个问题，与各自的根因：

1. **默认状态放下来很小**：默认配置的盒子是 6.5 × 1.5 板面格，而它声明的 footprint 是 8 × 4 格——
   可见区域比它自己占的地方小一圈。
2. **缩放不同步**：`layoutNote()` 里字号、内边距、编辑按钮都带**绝对像素下限**
   （`max(5,…)` / `max(2,…)` / `max(14,…)`）。当每格像素数（`unit`）掉到下限以下（按钮在 unit < 22.6、
   字号在 unit < 11.9）这些量就被地板顶住不再缩，于是"视野拉大时它变得不够小"；盒子宽度又取
   `max(声明宽度, 文本宽 + 内边距)`，文字不缩会连带把盒子撑住。

修法：所有像素量按 `unit` 成比例（只留亚像素 epsilon），默认配置按 footprint 定尺。

**证据**：`TC_TEXTBOX_LAYOUT_TRACE=1` 每秒把布局以"板面格"写进日志；同一次运行里经过三个相机缩放：

```text
unit=25.600 box=7.60x3.60 cells button=0.62 cells text=0.60 cells padding=0.22 cells   ← 默认配置的便签
unit=25.600 box=7.00x3.60 cells button=0.62 cells text=0.50 cells padding=0.22 cells   ← 自测便签（字号/宽度被自测覆盖）
unit=28.160 box=7.00x3.60 cells button=0.62 cells text=0.50 cells padding=0.22 cells
unit=13.137 box=7.00x3.60 cells button=0.62 cells text=0.50 cells padding=0.22 cells   ← 连缩 8 档后仍然一样
```

格子数在三个缩放下完全一致 → 盒子、编辑按钮、字号、内边距都跟着相机走。门禁里的硬断言（在
`TC_TEXTBOX_KEEP_SELECTION=1` 场景、日志里出现 ≥2 个不同 unit 时运行）:

```text
PASS text-box note layout stays 7x3.6x0.62x0.5x0.22 cells at 3 camera scales
```

**默认尺寸**：`defaultConfig()` 现在按 footprint 定尺——字号 0.60 格、最小宽度 = 2 · 半宽 − 0.4
（7.6 格）、盒子最小高度 3.6 格。空便签放下来就是 7.6 × 3.6 格，与"可拖区 8 × 4 格 + 选中提示矩形"
基本重合，"看到的就是能拖的"。默认值仍然每实例可改（编辑器里的"字号/最小宽度"滑杆），
旧的已存便签配置不受影响（配置是按实例存的，没动 schema）。

**顺带修的两处测试装置问题**（都是为了上面那条断言能跑）：

- `reportProbeCenters()`：默认绘制的探针中心以前只在平移后播报一次，之后的诊断性缩放会让像素窗口
  指向旧位置；现在相机一变就重新播报，门禁取最后一条。
- 默认绘制的像素窗口从 ±90×±30 px 收窄到 ±45×±18 px，阈值改成**随缩放的比例**
  （`150·(unit/25.6)²`，最低 20）：窗口必须落在便签自己的暗色盒子里，否则缩小时会把板面装饰的波形线
  也数成"青色像素"。当前输出
  `PASS text-box default drawing pixels hidden=0 visible=350 want>=181 unit=28.16`，
  缩小到 13.14 px/格 时是 `hidden=0 visible=82 want>=40`。

## 按类型去掉"在元件工坊编辑"按钮（2026-09-23 第五轮，`tc.component.render` V4）

问题：Mod 元件走的是游戏自定义元件那条路，所以选中它时，底部元件面板右上角会出现游戏的
**"在元件工坊编辑"**按钮（英文就是 `Foundry`，翻译 id `48920514303805`）。对 Mod 元件点它是没有意义的：
它的逻辑来自 Mod 的代码、外形来自 `tc.component.geometry` 声明的 footprint，工坊里那份"内部电路"
并不归玩家编辑。

**机制（静态跟读 + 真机）**：那个按钮是 `build_component_description_panel`
（@0x14039b400，整个二进制里唯一调用 `build_custom_component_preview` 的两处之一）里的一次
`igButton` 调用，**call site RVA 0x3a45ac**；点下去的处理是
`play_sound` → `append_level_return_stack` → `store_custom_foundry_inputs` → 把元件原理图名做成
目录名 → `load_level_frontend`（载入工坊）→ `restore_custom_foundry_inputs`。

**做法**：宿主只在那一个 call site 上动手——`igButton` 钩子里判断"这一次调用是不是那个按钮"，
如果是、且**当前选中的元件全都是**请求关掉该按钮的 Mod 类型，就用全透明的样式色画出它
（`Text`/`Button`/`ButtonHovered`/`ButtonActive`/`Border`），并返回"未点击"：
按钮既不可见也不可点，而面板布局与其它控件完全不变（按钮仍然占位，不留下空洞）。选中的元件里只要
有一个不是这些类型，按钮照常出现。

**真机证据**（同一个 42 × 42 px 的按钮矩形，同一张截图；矩形由钩子自己的
`igGetItemRectMin/Max` 报出）：

```text
默认（Mod 关掉）  PASS text-box foundry button pixels=0   rect=2507,1213..2547,1253 hidden=1
对照（保留按钮）  PASS text-box foundry button pixels=654 rect=2507,1213..2547,1253 hidden=0
日志              Component render: the foundry edit button is hidden for the selected Mod component
```

保留对照用 `TC_TEXTBOX_FOUNDRY_BUTTON=1`（Mod 不调用 V4），此时同一矩形的中心像素是纯白
`255,255,255`；关掉时是面板底色 `46,43,60`。两条断言都写进 `tests/text-box-playtest.ps1`
（`TC_TEXTBOX_KEEP_SELECTION=1` 场景，脚本自己打开 `TC_FOUNDRY_TRACE=1`）。

**边界**：只影响调用方自己登记的类型；选择、命中、拖动、删除、默认绘制、选中提示、footprint、
以及工坊本身（包括打开工坊的工具栏按钮）都不变。`abi/windows-x64.json` 基线 1192 → 1203 条，
新增记录全是 `TCComponentRenderApiV4`（`sizeof=48`），V1/V2/V3 一条未变。

## 文本框改回真 0 脚（2026-09-23）

结论：装饰元件**不需要**为了拿到实例而留一根悬空输出脚。真 0 进 0 出在本构建上照常进平坦序列、
照常有实例句柄，文本、绘制、拖动、编辑与像素门禁全部不受影响，而且游戏不再画引脚点。

- 形状：`examples/text-box` 现在用 `registerNote(0)`，真机日志
  `Native logic: registered custom 0x544558545f303031 inputs=0 outputs=0 shape=0in/0out v2 state=0`；
  2026-09-22 研究里"绑不上实例"的结论在本构建已不成立——脚手架给 0 输出补的那个驱动门是真节点，
  一次运行里 7 个实例（4 个便签 + 3 个对照实例）全部出现
  `Native logic: bound instance 0x… of custom 0x544558545f… as token N`。
  机制见 `src/component_definition.hpp` 的 `validShape` 注释与 `encode()` 的
  `if (m == 0) w.component(0x12, 4, 0, 0x4000, "", 1);`。
- 完整门禁：`./tests/text-box-playtest.ps1` 在同一沙箱里全绿——放置成功、`matched=4 notes=4`、
  `PASS text-box render transforms rotations=0xf`、`PASS text-box render zoom live unit 25.60 -> 28.16`、
  `PASS text-box render pan delta=150.00,0.00 axes unchanged instances=4`、
  `PASS text-box default drawing pixels hidden=0 visible=350 want>=181 unit=28.16`、
  `PASS text-box hidden note drag from -6,-18 to -5,-18 selection=1`、
  `PASS text-box hidden note deselected selection=0 cleared=1`、四边裁剪像素与编辑器自测照旧。
  编辑器的配置读回（`text-box autotest: configuration read back ok`）说明按「板面 + 元件 id」的
  `plugin-data/notes.txt` 存储与新形状兼容。
- 外观：关掉默认绘制的实例窗口现在是**全空**（上一轮还有游戏的粉色引脚点）；
  便签本体处也不再有点痕。对照图 `build/text-box-out/zeropin-note.png`（便签）与
  `build/text-box-out/zeropin-hidden.png`（关闭绘制的实例）。
- 回退开关：`TC_TEXTBOX_OUTPUT_PIN=1` 注册旧的"一根悬空输出脚"形状，保留用于对照。

## M5 render V2 关闭游戏默认绘制（2026-09-23）

结论：`tc.component.render` V2 的 `set_default_drawing(id, 0)` 已在隔离真机里逐像素验收，而且
**不改变** footprint、命中/拖动、选中/取消选中；元件栏预览不受影响。命令与读数如下。

```powershell
./build.ps1                                    # 离线：几何/渲染/契约单测与示例全部通过
./tools/abi.ps1                               # ABI：1182 条记录与基线一致
./tests/text-box-playtest.ps1                 # 真机：默认绘制像素 + 变换/缩放/平移/裁剪 + 拖动/取消选中
```

- 离线与 ABI：`tests/component-render.cpp` 输出
  `PASS component render SDK tables, default drawing and affine mapping`；V2 新增记录全部属于
  `TCComponentRenderApiV2`（`size/version/context/set_draw_callback/set_default_drawing`，共 32 字节），
  V1 记录一条未变，基线按 §20 评审后从 1173 条更新到 1182 条。
- 接管点：加载器钩住 `redraw_component__presenterZupdate95state95common_u4925`（日志
  `Component render: default drawing control armed`），只对"类型被关闭 + 带实例 id"的棋盘实例跳过
  这一次默认绘制，日志逐实例输出
  `Component render: suppressed game default drawing custom=6072356791976472625 instance=<id>`。
  元件栏/选择器的预览记录 id 为 0，因此不被跳过（未加实例判据时真机日志里出现过
  `instance=0` 的抑制行，是这条判据的来源）。
- 像素回读（同一沙箱、同一帧、两个同类型实例对照，进程内 BMP 逐像素计数，阈值 `hidden<=4`、
  `visible>=100`）：

  ```text
  PASS text-box default drawing probe centers hidden=1092.1,462.1 visible=1767.9,462.1
  PASS text-box default drawing pixels hidden=0 visible=350 want>=181 unit=28.16
  ```

  证据图：`build/text-box-out/hidden-probe.png`（关闭默认绘制的实例：只剩引脚点）、
  `build/text-box-out/visible-probe.png`（开着绘制的对照：青绿色缩略图板 + 水印）。
  这些图由 `build/text-box-out/crop.ps1` 从该次运行的 `board.png` 裁出；
  `build/text-box-out/count-pixels.ps1` 用同样的两种口径复算计数（可用来核对脚本自己的判据）。
- 命中/拖动不受影响：被关掉绘制的类型另放一个同类型实例，自动化在其声明 footprint 内、**距中心
  3 板面格**的空像素处按下并拖动一格；位置与游戏自己的选中计数都被读出：

  ```text
  PASS text-box hidden note drag from -6,-18 to -5,-18 selection=1 instance=2480352624375808058
  PASS text-box hidden note deselected selection=0 cleared=1
  ```

  断言写死在 `tests/text-box-playtest.ps1`：位移必须恰好一格，选中计数必须先为 1（拖动确实选中了
  它）、再由游戏的 `clear_selections__modelZboardZboard_u8323` 清回 0。
- 这一轮顺带查清一个读数陷阱：**拖动之后游戏会在被拖动的实例上画自己的选中提示 UI**（引脚外侧的
  白色断环 + 引脚标签），它属于游戏的交互提示层，不属于 V2 接管的默认绘制，也不随
  `clear_selections` 立即消失（实测截图 `build/text-box-out/hidden-drag.png`）。所以像素测量用的是
  从未被拖动的兄弟实例；否则那个提示环的抗锯齿像素会被算成"游戏默认绘制残留"。
- 回归开关：`TC_TEXTBOX_DEFAULT_PROBE=1` 放置两个对照实例，`TC_TEXTBOX_DRAG_PROBE=0` 跳过拖动阶段
  （两者都只影响这个探针 Mod）。跳过拖动时隐藏窗口的宽口径计数是 0，正是上面那条结论的对照实验。
- 顺带修掉一个既有偶发失败：编辑器那一次点击原来是同一帧里连发 `WM_LBUTTONDOWN`+`WM_LBUTTONUP`，
  游戏在一次采样前把对折叠掉，实测约每六次运行失败一次（`the click did NOT open the editor`）。
  现在按下与抬起分两帧发送（与相机拖动、拖动用例相同的形式），改后连续两次整用例通过。

## M5 render V1 覆盖层切片（2026-09-23）

- 离线：`tests/component-render.cpp` 验证服务表查询、回调登记封装、旋转后的二维仿射映射和 4096 条
  图元配额常量；输出 `PASS component render SDK table and affine mapping`。
- 构建：加载器完整 DLL 与 `examples/text-box` 均通过 MinGW 编译；`tools/abi.ps1` 通过，共 1213 条。
- 真机首个消费者：`tests/text-box-playtest.ps1` 的隔离棋盘日志同时出现
  `Component render: overlay primitives armed`、`type footprint registered half=4.0,2.0` 与
  `tc.component.render callback active`，并正常完成编辑器自测和进程内截图。回调只使用宿主仿射基
  及受控线图元画四边框。
- 四向变换：同一真机用例放置 rotation 0/1/2/3 四个实例；回调将四组轴逆旋回统一基底并比较，
  验证每个 8×4 局部框的四角位于有效裁剪框内，输出
  `PASS text-box render transforms rotations=0xf unit=25.60/25.60 clip=2560x1600`。脚本已把该行设为
  硬断言。
- 缩放变化：用例在棋盘空白处发送一格滚轮，旧帧轴长 `25.60 px/格`，新帧轴长 `28.16 px/格`，
  回调输出 `PASS text-box render zoom live unit 25.60 -> 28.16`，脚本同样硬断言。
- 平移变化：缩放稳定后保存四个旋转实例的屏幕原点和仿射轴，在空板执行一次 150 px 中键拖动；
  四个实例必须得到相同位移，且各自四个轴分量保持不变。实测输出
  `PASS text-box render pan delta=150.00,0.00 axes unchanged instances=4`，随后按更新后的屏幕坐标点击
  编辑按钮仍能打开目标实例。脚本对 PASS 行和编辑器结果均作硬断言。
- 裁剪像素：同一用例用测试专用 `TC_MODLOADER_RENDER_CLIP=800,200,1800,850` 把四条裁剪边移到
  无 UI 遮挡的板面，回调持续绘制四个跨边不透明色块。进程内 BMP 回读后逐像素找色，输出
  `PASS text-box render clip pixels left=800..831 right=1768..1799 top=200..231 bottom=818..849`。
  这同时证明回调收到的 clip 与宿主实际提交给 ImDrawList 的 clip 相同；任一方向越界泄漏、错误地
  裁掉框内像素或没有画出标记都会失败。
| 真机沙箱用例 | `tests/*-playtest.ps1` | 元件导入/放置/保存、声明延迟、原生逻辑回调 |

`tests/test-catalog.json` 是自动化清单的单一入口：测试 ID、层级、执行器、超时、是否需要
指定游戏构建都写在这里。`tools/test.ps1` 校验清单后串行执行，按用例保存 stdout/stderr，
并在 `build/test-results/` 生成 `results.json` 与 JUnit `results.xml`。人工用例带
`manual: true`，即使 `-Tier all` 也不会意外弹出可见游戏窗口。

## 怎么跑

```powershell
# 不需要游戏二进制：完整构建、编译型单测、包管理与存档模型
./tools/test.ps1 -Tier fast

# 需要指定构建的文件、但不启动真实游戏 UI：原生宿主、钩子链与安装器
./tools/test.ps1 -Tier host

# 真机沙箱回归；自动先构建，已有新鲜构建产物时可加 -NoBuild
./tools/test.ps1 -Tier game

# 所有非人工用例
./tools/test.ps1 -Tier all

# 发现／定点运行
./tools/test.ps1 -List
./tools/test.ps1 -Name native-logic -NoBuild
./tools/test.ps1 -Name 'ui-board-panel*' -NoBuild
```

默认遇到第一项失败就停止；调查多个独立失败时加 `-KeepGoing`。真机用例仍可直接运行原脚本，
例如 `./tests/custom-or-playtest.ps1 -Scenario shape_adder8`。目录中的 probe→driver 顺序就是
全量执行顺序，因此板侧栏和键盘用例会先记录真实矩形，再发输入。

## 公共沙箱模块

`tests/lib/Sandbox.psm1` 统一负责可复用的游戏副本、Mod 部署、独立用户目录、进程收尾和
等待结果文件。它会在复制前检查游戏文件与构建产物，并拒绝删除 `build/` 之外的目录。
`tests/make-ui-sandbox.ps1` 已改用该模块，所以页面、绘图、纹理、键盘、板侧栏和波形等
共用这个准备入口的测试不再各自维护复制规则。其他历史用例保持原行为，后续修改到它们时
再逐步迁移，避免一次性改写已经验证过的真机断言。

## 显示、窗口尺寸与 DPI（2026-09-18）

## 宿主契约：能力协商与依赖版本（0.6.0）

对应实现见 [reference/capabilities.md](reference/capabilities.md)；这里只记"凭什么算过"。

**离线**（`build.ps1` 内执行，`tests/capabilities.cpp`）：

| 断言 | 说明 |
|---|---|
| 能力表自洽 | 每个位都能按名字找回（`capability_bit(name)==bit`）、没有重复位、`capability_names()` 覆盖全部位、名字全小写无空格 |
| 版本只有一处 | 读仓库 `VERSION` 与 `TC_MODLOADER_VERSION_STRING` 逐字比较，并核对 `TC_MODLOADER_VERSION_CODE` 与 `TC_HOST_VERSION_CODE` 一致 |
| 版本序 | `1.10.0 > 1.9.0`（字符串比较会弄反）、`1.0 == 1.0.0`、`v2.0.0 == 2.0.0`、`1.0.0-beta == 1.0.0`、纯文本回退字符串比较 |
| 约束语义 | `*`/空/相等/不等/四个比较/逗号联合；`>=` 缺操作数、`1.0.0` 对空版本等一律**拒绝**而不是放行 |
| 依赖两种写法 | 数组与对象写法都解析；重复 id 只留第一次；非列表、非法 id、非字符串约束分别报错；缺键不是错误 |
| 能力声明的四种拒绝 | 未知名、加载器不提供、非数组、非字符串条目；未知名与"不提供"是两条不同消息 |
| 文档一致 | `docs/reference/capabilities.md` 必须出现表里每个 `` `name` ``，防止文档与代码漂移 |

**脚本**（`node tests/run.js`，共 35 项，其中 11 项本轮新增）：CLI 的
`--version` 与 `VERSION` 一致、`--capabilities` 名单、能力声明被接受、未知名在扫描期被拒
（`list` 显示、`apply` 拒绝）、约束满足/不满足（且**磁盘无改动**）/数字序/可选依赖缺失可
接受/可选依赖一旦启用仍检查/约束写成数字被拒。

**真机**（`tests/ui-board-panel-playtest.ps1`）：`example.board-panel` 现在加载期先
`tc::hostHas(h, TC_CAP_UI_SLOT)` 再注册，并调用 `tc::reportStatus`。日志证据（沙箱
`loader.log`）：

```text
TC Mod Loader 0.6.0 capabilities: log, symbol, hook, logic, component, ui_page, ui_slot, texture, status
[dev.board-panel-driver] status: Board panel: registered slot 'main'
Native loaded: dev.board-panel-driver; hooks=7
```

第二行同时证明两件事：插件读到了 `capabilities`（否则它按新代码直接返回 2、面板不会注册，
测试也不会通过），并且 `report_status` 的结果**没有被**加载器随后的"运行中"覆盖。

**清单全量对照**：把 `dist/*.mod` 全部放进一个临时目录跑
`tcmod-cli <dir> list`，29 个包（含所有示例与开发探针）都没有能力相关错误——
每个示例的 `capabilities` 声明与它实际调用的入口一致。

## 符号画像与钩子链（0.6.0）

**离线**（`build.ps1` 编译 `tests/hook-chain.cpp`，`tests/hook-chain.ps1` 驱动）：
测试宿主自己定义与真机同名的 `sim_do` / `sim_get_cycle` / `load_level` 假符号，
探针包（`tests/hook-chain-probe.cpp`）加入链并把每一步通过 `tc_test_note` 报回宿主。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/hook-chain.ps1
```

| 场景 | 断言 |
|---|---|
| 顺序与参数 | 优先级 0 的 `a0`、同优先级但 Mod id 更大的 `b0`、优先级 10 的 `a10` 依此运行；`a0` 看到 1000，`a10` 看到 1002（前一个链节的修改传下去了），游戏自身函数拿到 1003 且只调用一次 |
| 吞掉原函数 | 链节置 `skip_original` 后，后面的链节不再运行，游戏自身函数一次都没被调用 |
| 链点保留 | 普通目标（`sim.cycle`）原始 Hook 成功；对 `sim.do` 的原始 Hook 被拒 |

日志证据（探针包与加载器各写一部分）：

```text
Hook chain sim.do joined (priority 0)                       <- 插件侧
Hook chain sim.do installed with 4 link(s): @-2147483647, dev.hook-chain-a@0, dev.hook-chain-b@0, dev.hook-chain-a@10
PASS hook chain: three links ran in priority/mods order, edits reached the game function, and it still ran once
```

最前面的 `@-2147483647` 是加载器自己的链节（仿真控制与 `sim.do` 事件），2026-09-27 更新时把用例的断言
从"恰好 3 个链节"改成"三个 Mod 链节都在、且按优先级顺序"，避免加载器自己加链节就让用例变红。

**脚本**（`tests/native.ps1`）：冲突场景改成两个包抢同一个普通目标——第一个
`raw plain-hook ok=1`，第二个 `Hook rejected: phase, target or conflict` 且 `ok=0`；
同一脚本的正常/篡改/禁用三种场景继续用 `example.cycle-guard` 走链拦截
（`100000 -> 200`）。

**真机**（两个沙箱，各自独立存档）：

| 用例 | 证据 |
|---|---|
| `tests/waveform-playtest.ps1` | `Symbol profile: 20/20 aliases resolved`；`Hook chain level.load installed with 1 link(s): dev.waveform-demo-driver@0`；波形、VCD、导线探针断言全过 |
| `TC_SIM_STATE_EXTRA_MODS=example.cycle-guard` + `tests/sim-state-playtest.ps1` | 同一个 `sim.do` 点上有两个链节：`Hook chain sim.do installed with 2 link(s): dev.sim-state@0, example.cycle-guard@0`，关卡跑完、状态映射断言全过（`input_replay slot 0/8`、`output_history slot 55/64`） |

`Symbol profile: 20/20` 是符号画像的正面证据：表里**每一个**别名都在真机构建里解析成功；
解析失败的项会逐条写成 `Symbol profile: unresolved <别名> (<COFF 名>)`
（离线测试宿主只定义 4 个假符号，日志里就能看到另外 16 条，说明缺失不是静默的）。

## 事件总线（0.6.0）

**离线**（`tests/hook-chain.ps1` 的 `events` 模式）：测试宿主额外提供与真机同名的
`change_scene` / `save_level_data` / `save_count` 假符号，探针包订阅全部四种事件。
断言：四种事件都到达同一个监听器、顺序与调用顺序一致、负载正确
（`event sim.command command=0`、`event level.load subject=0`、`event scene.change`、
`event save count=3`），并且仿真命令仍然原样送到游戏自身函数（监听器不改变行为）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/hook-chain.ps1
```

加载器侧同时断言四个事件源都被装载（否则事件不可能到达）：

```text
Event source level.load: ok
Event source sim.do: ok
Event source scene.change armed (Board handle tracking)
Event source save armed
PASS event bus: sim command, level load, scene change and save reached one listener, in order, without disturbing the game
```

**真机**（`tests/waveform-playtest.ps1`）：`example.waveform-demo` 现在订阅关卡加载事件拿板模型
（不再自己进 `level.load` 链），波形、VCD 与导线探针断言全过；日志：

```text
[dev.waveform-demo-driver] Waveform: board model capture subscribed to the level-load event
Event source level.load: ok
Hook chain level.load installed with 1 link(s): @-2147483647     <- 加载器自己的链节
```

## 游戏对象句柄（0.6.0）

**离线**（`build.ps1` → `build/game-handles-test.exe`）：签发、解析、kind 拒绝、代次失效、
棋盘场景进入与离开。关键一条是真机探针逼出来的：关卡加载**同一帧**的场景切换属于“进入”，
必须保留句柄；之后的切换才算离开。

**真机**（`tests/game-handle-probe-playtest.ps1`，游戏层用例 `game-handle-probe`）：
`dev.game-handle-probe-driver` 按主页自带的关卡入口进入关卡、按 **Escape**（玩家自己的离开
动作，不是驱动造出来的切换）让游戏自己调用 `change_scene(ctx,0)`，然后再进一次、再退一次；
插件只通过公开 ABI 报告。一次真实运行：

```text
PROBE: Board unavailable on the main menu (unavailable)
DRIVER: pressing the level entry (rva=0x44a308) to reach the first level
PROBE: level.load name=sandbox subject=0x334c7e2300 frame=270
PROBE: level handle generation=2 token=1 size=24 resolve=ok resolve-match=1 valid=1
Game handles: scene 1 in the level's own frame; the Board handle stays valid
PROBE: Board available generation=3 token=2
DRIVER: trying the game's own way out (Escape)
PROBE: scene.change scene=0 frame=473 old-handle-valid=0
PROBE: old handle resolve=stale pointer=cleared
PROBE: current after scene change=unavailable
DRIVER: self-exit=ok
DRIVER: the game left the level by itself and the old handle was invalidated
DRIVER: pressing the level entry (rva=0x44a308) to reach the second level
PROBE: previous handle valid=0 after the new level load
PROBE: level handle generation=5 token=3 size=24 resolve=ok resolve-match=1 valid=1
DRIVER: trying the game's own way out (Escape)
PROBE: scene.change scene=0 frame=1076 old-handle-valid=0
DRIVER: self-exit=ok
DRIVER: the game left the second level by itself and its handle was invalidated
DRIVER: done
PASS Board handles: ...
```

同一用例第二次进入也打印 `name=sandbox`；脚本明确拒绝 `(none)`。事件里的名字来自钩子参数的
Nim 字符串，转换后的 C 字符串只在回调期间借用。

这个用例是**唯一**发现“进入关卡的场景切换把句柄当场作废”的地方：离线注册表测试没有帧概念，
假宿主也不会真的切场景，所以只有真机生命周期能暴露它。同一次运行还确认了
`scene.change` 在加载插件之前就被加载器自持（`Event source scene.change armed (Board handle
tracking)`），插件侧的 `create_hook` 会被拒绝并提示改用事件。

`self-exit=ok` 是硬断言：如果某个构建里 Escape 不再离开关卡，脚本会失败并提示去看日志，而不是
静默走驱动自己的 `change_scene` 回退分支——回退路径只保证“加载器这一段是对的”，不能证明
“玩家的动作会走到这里”。

### Board V4 对象数据读取

**离线**（`build.ps1` → `build/board-objects-test.exe`）：合成记录验证序列读取、元素成员判定
（序列头／越界／错位／另一个数组的元素都拒绝）、元件与导线字段解码，以及“宽度或状态槽超出实测
范围时只清 flag、不发布数值”。`tests/services.cpp` 另加 V4 表检查：版本、前缀与 V3 一致、
未绑定读取器返回 `UNAVAILABLE`、短缓冲区拒绝、句柄原样透传。

**真机**（同一个 `game-handle-probe` 用例，一次真实运行）：

```text
PROBE: board v4=4 prefix=1
PROBE: objects status=0 components=3 wires=1 written=4 resolved=4 generation=306
PROBE: component heads [0]=0000…0000 [1]=3c00f6ff00000000f8b411d75f2c4c04… [2]=44000a0000000000010000…
PROBE: component kinds 0:1 60:1 68:1
PROBE: component info status=0 read=3 of=3 unique-ids=3 custom=0 kind0=0 xy0=0,0 rotation0=0 id0=0
PROBE: wire info status=0 read=1 of=1 endpoint=1 width=1 slot=1 xy0=9,-9 width0=1 slot0=256
PROBE: child read next-frame component=-3 wire=-2
PROBE: settled objects status=0 components=1 wires=0 read=1 unique-ids=1 empty=1 kinds=0:1
```

这轮的价值集中在三条**只有真机能给出**的结论：

1. 导线字段与既有文档一致：`slot0=256`、`width0=1` 正是 waveform 交接文里记录的实测值，说明
   `+0x30`／`+0x38` 的解释在服务层复现成功。
2. kind `0x3c`／`0x44`、坐标 `-10`／`+10` 与仓库自己的 fixture 吻合（`tests/and-component-fixture.cpp`
   里 `0x44` 就是关卡输出引脚），说明元件头部字段没有错位。
3. 最初按“第一条记录就是元件”写的断言在真机上**当场失败**：元件序列第 0 条是全零占位记录
   （kind 0，游戏 kind 表里没有 0）。断言改为按 kind 直方图判定，并新增“`level.load` 帧内的棋盘
   还没稳定（3 元件／1 导线 vs 相邻帧 1／0）”这条观察，两条都写进了 [sdk/services.md](sdk/services.md)。

导线端点读法也被真机修正过一次：旧读法把 `+0x18`／`+0x1c` 当成一个端点的 x/y，战役关卡的探针
打印 `xy0=9,-9`（其实是 x1 与 x2）；改成两对 int16 后同一块板子打印
`xy0=9,0->-9,0`——一条横在 (-10,0) 输入引脚与 (10,0) 输出引脚之间的线，几何自洽。判据与另外两组
证据见 [research/board-object-fields.md](research/board-object-fields.md) §4.1。

**加线入口的边界（真机实验，故意不做成命令）**：`add_wire_from_pos` 放线后棋盘 0→1 条，
记录是 `w(10,10,w1,slot1)`（两个端点相同、奇数槽），而游戏自己的
`get_wire(model,(10,10))` 返回 `INVALID_WIRE_ID`——它只是"起一条线"。因此 `PLACE_WIRE` 不进
命令契约，理由见 [sdk/commands.md](sdk/commands.md)。

### Board V5 元件引脚

**离线**（`build.ps1` → `build/board-pins-test.exe`）：用合成的 `PROTOTYPES` 形状表验证内置 kind
枚举的三条护栏（长度上限、空桶、缺失桶指针），并用合成的引脚条目验证解码——坐标必须从
`TCPin* + 8 + 2` 读（测试故意在条目 `+2` 放了一个诱饵值），`AUTO_SIZE` 只置 flag 不报位宽。
`tests/services.cpp` 另加 V5 表检查：前缀与 V4 一致、未绑定读取器 `UNAVAILABLE`、容量不足
`CAPACITY`、短缓冲区拒绝、缓冲区原样透传。

**真机**（`game-handle-probe`，一次真实运行）：

```text
Board pins: prototype reads armed, 125 built-in kinds
PROBE: pins 1 kind=0x3c in=0 out=1 p0=o(1,0,w1)
PROBE: pins 2 kind=0x44 in=1 out=0 p0=i(-1,0,w1)
PROBE: component pins status=0 read=3 of=3 pins=2 expected=2 zero=1 auto=0
```

`125 built-in kinds` 与 `build/kinds.txt` 的行数一致；两行引脚与同一构建的 `0x3c "Input"`／
`0x44 "Output"` 条目逐字段相同，因此这一次运行同时盖章了原型查找、描述符锚点和字宽字段。
`zero=1` 是那条 `kind == 0` 的占位记录：它在游戏的 `PROTOTYPES` 表里是空原型，所以服务照实返回
0 个引脚——正确的做法是照游戏回答，而不是在服务层发明一条"特殊拒绝"。

**自定义原型分支**（`component-placement-playtest`，真机）：探针从菜单路径放一个 fixture AND2，
再用 V3 枚举 → V4 认身份 → V5 读引脚，断言
`inputs=2 outputs=1 kind=0x4e` 与 `p0=i(-1,-1,w1) p1=i(-1,0,w1) p2=o(2,-1,w1) written=3`。
这条补上了 V5 唯一没有真机覆盖的分支（战役关卡里没有自定义实例）。

### tc.simulation 仿真读数

**离线**（`build.ps1` → `build/simulation-test.exe`）：掩码与界内算术的边界（`bits` 1/63/64、
偏移正好在缓冲末尾／越界一字节／超大偏移）、服务表版本与短缓冲、未绑定读取器
`UNAVAILABLE`、旧宿主经 SDK 辅助查询返回 `UNAVAILABLE`。

**真机**（`game-handle-probe`，一次真实运行）：

```text
Simulation reads: armed, state buffer 10240000 bytes
PROBE: sim state status=0 cycle=-1 frame=134 state-size=10240000 flags=7
PROBE: sim value status=0 raw-status=0 slot=256 width=1 value=0 raw=0 masked=0 agree=1
```

这两个关卡里探针没有启动仿真，所以 `cycle=-1`、值为 0：这一层验证的是"别名已武装、偏移在界内、
掩码规则与游戏一致"，**不是**值的正确性。值随周期变化的正确性由 `waveform-playtest` 负责——它跑
同一个读取函数（`sim_state_read_u64`），并把探针值与关卡自带的输出历史逐行比较（见上文波形一节）。

### tc.io_value 数值编辑器（离线已覆盖，真机随首个使用方）

**离线**（`tests/io-value-service.ps1`，fast 层，`tests/io-value-service.cpp`）：Loader 侧的
表达式解析器（十进制、`0x`/`0b`/`0o`、`~` 与一元 `-`、`* / %`、`+ -`、`<< >>`、`& ^ |`、
括号、数字间 `_`）逐条断言，含 `0xFFFFFFFF^(1<<23)`、`~(1<<23)`、`(0xF<<8)|0x3`、`1<<64`
归零、`0x1FFFFFFFFFFFFFFFFF` 报 `RANGE`、`1/0` 与 `(1` 等非法式报错；位宽截断
（1/8/64/0 位）；服务表形状（`size`/`version`/`context` + 八个入口，缺一个就不 ready）与
`TC_SERVICE_IO_VALUE` 的 id 字符串。

另外 `tools/abi.ps1` 已把 `TCIoValueApiV1` 与 `TC_IO_VALUE_*` 常量纳入基线
（`abi/windows-x64.json`，本次是**纯新增**：原有记录一条都没有改值，只补上了服务机制引入的
`sizeof.TCHost=208` 与 `offsetof.TCHost.query_service=200`）。

**真机**：本服务的读写入口与现有 Mod 走的是同一条游戏函数（`get/set/flip_component_global_input`、
`set_setting` + 运行时值槽 + `sim_stop_and_refresh`），后者已有真机证据（见打孔纸带一节）；服务
自己的端到端验收安排在第一个使用方——打孔纸带的掩码功能——落地时一并跑，断言"表达式求值 → 写值
→ 从 `#SIMULATION_STATE` 回读"三者一致。

### tc.pin_order 引脚顺序（离线 + 真机拖拽）

**离线**（`tests/pin-order.ps1`，fast 层，`tests/pin-order.cpp`）：用一个与游戏同形状的假面板
缓存（context 里三段 `{count, payload}`、payload 头部的条数、32 字节的
`{元件下标, 位宽, 名字{长度, 头部+字符}}` 记录）断言：`count`/`entry` 读到的 key、位宽、名字；
`move(0,0,2)` 后活缓存被重排成 `3,4,2` 且另一组不动；重复 `apply` 不会二次置换（顺序是按 key
表达的）；同一组引脚重建后顺序保留；换成另一组引脚时顺序失效并回到面板自己的次序；
`set_order` 拒绝面板没有的 key；面板不在时 `move` 返回 `STATE` 且不会猜。
同一 fast 用例还断言 V2 条目布局会把同帧同 key 的多个控件矩形取并集、保留上一完成帧供外框
读取，并清理更老的帧；`tests/services.cpp` 另行检查 V1/V2 前缀一致、未知版本拒绝与
`NOT_FOUND` 语义。

**真机**（`tests/pin-order-playtest.ps1`，game 层）：

- `-Probe` 用 `tests/pin-order-probe.cpp` 打印面板每条锚点（`igSetCursorPos` 的调用点、窗口内
  坐标与屏幕坐标）以及 `tc.pin_order` 的 `count`/`entry`/`order` 回答——边框几何与"服务看到的
  引脚"由此对齐；`-Dump` 让 Loader 打印原始记录（`TC_MODLOADER_PIN_ORDER_LOG=dump`）。
- `-Drag` 用 `tests/pin-order-driver.cpp` 按 Mod 自己记录的条目矩形，向游戏窗口投递真实的
  鼠标消息（移动 → 按下 → 按住移动 → 松开），随后回读顺序。一次真实运行（关卡 `byte_adder`）：

```text
pin order: entry frame hook installed
pin order: dragging inputs entry #0 (key 2) at (33,355) panelHovered=1 anyItemActive=0
pin order: moved inputs #0 (key 2) to #2 (status 0)
pin order driver: drag inputs #0 -> #2; before=2,3,4
pin order driver: order inputs before=[2,3,4] after=[3,4,2]
Order: 2,3,4 -> 3,4,2
```

  截图 `build/pin-order-out/geo3.png`（`byte_adder`）与 `geo5.png`（32 位引脚的
  `symphony_8_io_devices`）确认边框贴合"标签 + 位方块 + 数值"，且抓取条只在行左边缘。

### 元件类型目录 `tc.component.registry`（离线 + 真机）

**离线**（`tests/component-registry.ps1`，fast 层，`tests/component-registry.cpp`）：直接驱动目录
存储，断言声明式注册保留引脚名/代价/owner；桥接返回的形状只覆盖位宽、不丢声明名；
被拒绝的类型以 `active=0` + 原因留在目录里，且**后到的笼统原因不覆盖第一条**；同一 id 之后注册
成功会替换该记录；被拒绝的 Mod 的类型被删除；每方向引脚列表被截到 32；目录满 256 后拒绝增长；
服务表形状（`size`/`version` + 四个入口）与 `TC_SERVICE_COMPONENT_REGISTRY` 的 id 字符串。

**真机**（`tests/component-registry-playtest.ps1`，game 层）：沙盒里同时加载一个必然被拒绝的探针
（九输入定义）与 `example.byte-adder`，探针在帧回调里打印整个目录。一次真实运行
（`build/component-registry-out/reg1.png`）：

```text
component registry: registering nine inputs returned -2 (refused)
component registry: catalogue: 2 type(s)
component registry: type id=dev.component-registry-probe/0x4241445f53484150 owner=dev.component-registry-probe name="Registry probe (nine inputs)" 0in/0out cost=0 delay=0 active=0 caps=0x0 status="the definition is malformed (name, pins, widths or counts)"
component registry: type id=example.byte-adder/0x414444385f303031 owner=example.byte-adder name="Byte Adder" 3in/2out cost=1 delay=1 active=1 caps=0x7
component registry: pin id=example.byte-adder/0x414444385f303031 in[1] name="A" bits=8
component registry: pin id=example.byte-adder/0x414444385f303031 out[1] name="Carry out" bits=1
```

`caps=0x7` = `LOGIC|WIDE_PIN|MULTI_PIN`；被拒绝项没有能力位。用例同时断言
`byte-adder: autotest loaded level`，证明它是在一个真正加载了关卡的会话里跑的。

### 引擎游标锚点：钩子链的两个新点（离线 + 真机）

**离线**（`tests/hook-chain.ps1` 的 `cursor` 场景，`host` 层）：`tests/hook-chain.cpp` 里
两个假引擎函数带真实导入名（`igSetCursorPos` / `igSetCursorPosY`），一个"假面板"函数调用它们。
探针（`tests/hook-chain-probe.cpp` 的 `TC_PROBE_CURSOR`）加入两条链，断言：

- 链交给链节的 **caller** 认得出面板（`cursorY panel=1 y=70`），面板以外的调用认不出
  （`cursor panel=0 x=3 y=4`），且游戏函数真的各被调用一次 / 两次；
- 链节改的 `y` 送到了游戏自己的函数（`cursorLineY == 71`）；
- 面板以外的调用没有被改（`cursorY == 4`）。

同一次运行还确认 `Hook chain ig.set_cursor_pos installed with 1 link(s): dev.hook-chain-cursor@0`
（加载器只装一个 detour）。SDK ABI 基线新增两个参数结构与两个 id，并记录
`sizeof.TCHookCall` 56 → 64（`caller` 追加在末尾，旧插件的偏移不受影响）。

**真机**（2026-09-21）：`tests/pin-order-playtest.ps1 -Level symphony_8_io_devices
-ExtraMods local.punch-tape` 一次运行里两条链各有 2 个链节
（`local.punch-tape@0`、`local.pin-order@100`），纸带把下面的内容整体推下去
（`punch tape: entry pushed down by 93 px`），截图 `build/pin-order-out/chain1.png` 显示
"输出状态"不再被压住；同时 `tests/punchcard-playtest.ps1 -PunchTape`（纸带自身）与
`tests/pin-order-playtest.ps1 -Drag`（拖动重排，`Order: 2,3,4 -> 3,4,2`）都通过。

### 命令 V2：通过总线放置元件（真机）

`component-placement-playtest` 先用菜单助手放一个元件，再通过已核实的低层删除入口把它删掉；
删除留下 kind 0 墓碑，所以随后的 `tc.commands` V2 放置复用原槽，而不是增长序列。一次真实运行：

```text
Board edits: component placement armed
component deletion candidate before=2 after=2 live=1->0 found=0
component placement command submit=0 request=1 before=2
component placement command complete state=3 result=0 submitted=617 completed=617
component placement command board before=2 after=2 live=0->1 found=1
```

离线部分（`build.ps1` → `build/board-edits-test.exe`）只覆盖能离线验证的那半：放置记录的 0x238
字节模板（kind／打包坐标／rotation／自定义 ID／两个容器默认值，以及模板之外必须全零）、极值坐标
`(-32768,32767)`、内置与自定义两种 kind 的映射。**记录是否被游戏接受**只能在真机上验证，所以那
一半由上面三条断言承担——`found=1` 是按公开路径（V3 枚举 → V4 读数）回读的，不是命令自己的返回值。

同一条链上随后走 `TCTransactionApiV2` 两步事务（连放两个元件），再做一次 undo/redo，这样能区分
"整个事务一步撤销"与"每条命令一步"：

```text
component placement transaction complete state=4 result=0 staged=2 completed=2
component placement transaction board before=2 after=4 found=2
component placement undo state=3 result=0 components=4 live=2 transaction-after=4 transaction-before=2
component placement undo points=30,0:0 20,6:1 24,10:1 28,10:0
component placement undo components [0x00(0,0)] [0x4e(20,6)c] [0x4e(24,10)c] [0x00(0,0)]
component placement redo state=3 result=0 components=4 live=3 restored=1
```

两条只有真机能给的结论：**一次 undo 只回退一步**（`(28,10)` 没了、`(24,10)` 还在），以及**被撤销的
元件留下 kind 0 的墓碑而不是缩短序列**（所以 `components` 仍是 4、`live` 才是 2）。低层删除也
遵循同一墓碑规则；这三条直接影响所有"数元件/做撤销"的 Mod，已写进文档。

### 生命周期结构变化事件（真机）

同一个 `game-handle-probe` 用例在**自身事务完成后**（避免与 stop+save 事务抢同帧导致 CONFLICT）
通过命令总线放一个内置 AND，落点 `(0,-12)`：

```text
PROBE: edit submit=0 request=2 live-before=0
PROBE: edit state=3 result=0 live-before=0 live-after=1
PROBE: lifecycle objects-changed=1 selection-changed=0 entered=1 left=0
```

`live-before=0` 是稳定帧的读数（板上只有 kind 0 占位记录），放置后 `live-after=1`；
`objects-changed=1` 说明结构指纹变化在下一帧被观察点发现并派发了**恰好一次**。两次踩坑值得记下：
① 一开始在 `level.load` 帧里读 before 并提交编辑，读到的是"还在构建中"的棋盘、而且让探针自己的
事务变成 CONFLICT；② 编辑必须在事务之后提交，读数才自洽。

### Board V6 连接判定（真机）

同一个 `game-handle-probe` 用例对战役关卡那条线做了连接解析，断言的是**几何预测值**而不是服务自己
的返回值：

```text
PROBE: board v6=6 prefix=1
PROBE: wire ends status=0 end0=(9,0,dir0,pin0,kind=0x44) end1=(-9,0,dir1,pin0,kind=0x3c)
```

预测来自三处独立数据：元件坐标 `(±10,0)`（V4 读数）、引脚偏移 `0x44 in0=(-1,0)` 与
`0x3c out0=(1,0)`（`build/kinds.txt` 真机 dump）、以及导线端点 `(9,0)->(-9,0)`（V4 两端点）。
离线侧（`tests/board-pins.cpp`）用同一组数字覆盖了匹配与邻近窗口的边界。

两次进入用的是**同一个关卡入口**，原因写在这里免得下次再花一轮去查：用
`TC_HANDLE_PROBE_TRACE=1` 逐条试过主页的五个关卡方格，#3、#4 按下后只是打开该关卡自己的界面
（那里测到的按钮是 `Reset` 和一个空标签按钮，再按后者也不会 `level.load`），#5 会让**游戏自己
以退出码 0 结束**，只有 #2 真正加载关卡——也就是说这是干净存档的解锁状态，不是探针的能力问题。
所以探针验证的是“玩家再次进入关卡”这条生命周期事实（新代次、新 token、旧句柄被拒），而
“进入另一张关卡地图”需要一份已解锁的存档；驱动里的入口是按调用点 RVA 精确按下的，换成别的
存档同样成立。

`tests/ui-board-panel-playtest.ps1` 的“切场景后棋盘停止绘制”步骤同时恢复：驱动改从
`TC_EVENT_SCENE_CHANGE` 的 `subject`（即游戏自己传给 `change_scene` 的 context）取值，
`DRIVER: board stopped drawing after the scene change; panel frames=849` 重新出现，脚本里
对应的 NOTE 分支已改回硬断言。

## 仍未做到：逐周期回调

目标：仿真跑得比渲染快时（"连续运行"）每个周期都能被采样，而不是跨周期丢点。

查证（本次，结论是"不能靠挂钩子做到"）：

```powershell
nm "Turing Complete.exe" | Select-String "step|cycle|tick"        # 只有 UI 的 next_cycle / 设置项，没有每周期步进函数
nm "Turing Complete.exe" | Select-String "compile95thread"        # 只有 sim_do / sim_get_cycle / sim_stop_and_refresh / compile_thread*
nm "compile.dll"        | Select-String "step|cycle|tick|run"     # 只有 GC/注册表相关的 cycle
```

`sim_do` 是"请求运行到第 N 周期"，真正的循环在 `compile_asm` / `compile_isa` 生成的机器码里
（这也是原生逻辑回调只能在生成源码里插入调用、而不是挂在某个函数上的原因）。
EXE 里没有可以钩的步进函数，因此**逐周期回调需要改走生成源码注入**——与
[sdk/custom-logic.md](sdk/custom-logic.md) 的桥接同一类工作，另立一项。

## 安全模式与崩溃隔离

目标（地基第四项的一半）：插件回调里的非法访问不应该带走整个游戏。

**结论：没有做到，只做到"崩溃署名"**。过程与实测如下，免得下次重新踩一遍。

1. MinGW-w64 的 GCC **没有** `__try`/`__except`（实测：`'__try' was not declared in this
   scope`），所以第一版用 VEH + `setjmp/longjmp`。
2. 该机制在孤立环境里**可行**（`tests/fault-guard-probe.cpp` + `tests/fault-guard-helper.cpp`，
   `build.ps1` 内执行，四种情形全部恢复并继续运行）：
   同模块内故障、经 MinHook detour 的故障、在所加载 DLL 内故障、以及"经 MinHook patch
   的 EXE 函数调用 DLL 内故障"的组合（最后一项正是钩子链的调用形态）。
3. 但在加载器里**不可行**：把守卫接到链节回调上，处理函数确实执行（`fault.log` 写下了
   `access violation at 0x…`），进程随后仍然以 **`0xC0000428` /
   `STATUS_INVALID_IMAGE_HASH`** 结束。为排除"插件 DLL 的锅"，又做了一次对照——把故障
   点放进**加载器自己的受保护代码块**（`TC_MODLOADER_FAULT_TEST` 临时探针），结果完全相同。
4. 因为"一半时候管用"的安全网比没有更危险，最终交付的是**可依赖的那一半**：
   `src/fault_guard.hpp` 只登记归属（哪个 Mod、哪个回调/钩子点、什么异常、访问地址），
   然后放行异常让进程照常崩溃。真机/离线验证见下。

要真正隔离，需要换一条路（本次未做）：用 MSVC 编译一个小 shim 提供 `__try/__except`
（项目当前只有 MinGW 工具链），或者在生成代码的边界上捕获（原生逻辑桥已经会往生成源码里
插调用，那里是唯一能拿到"每周期/每次外调"结构的地方）。

**验证**（`tests/hook-chain.ps1` 的 `crash` 模式）：探针包 `dev.hook-chain-crash` 的链节
故意写空指针；脚本要求进程**非 0 退出**（和没有加载器时一样），并且
`tc-modloader-data/fault.log` 同时包含 Mod id、钩子点与异常原因：

```text
dev.hook-chain-crash in sim.do: access violation at 0x0000000000000008
```

`src/loader.cpp` 的日志轮转（8 MiB，保留 `loader.log.1`）在 `tests/native.ps1` 与各真机
用例的日志检查中一并覆盖（日志行格式未变，所以已有的断言不受影响）。

加载器现在每次启动记一行显示诊断（`Display: …`），因为这台机器本身就跑在缩放桌面上：

```
Display: dpi-awareness=per-monitor-aware window-dpi=168 monitors=1
         screen=2560x1600 client=2560x1600 surface=2560x1600 ratio=1.000000,1.000000
```

`window-dpi=168` 就是 175% 缩放，进程是 per-monitor-aware（EXE 里没有 DPI 清单，说明是
运行时设置的）。也就是说所有点击/几何断言**本来就是在 175% 缩放的桌面上跑通的**；
`ratio` 是 ImGui 画布尺寸与 OS 客户区尺寸的比，正是测试把 ImGui 坐标换算成点击坐标时用的
那个系数，写在日志里便于以后对照。

窗口尺寸变化是自动化能覆盖的那一半，`tests/ui-board-panel-playtest.ps1` 新增一段：

1. 面板已经打开、滚动条也拖过之后，驱动把游戏窗口从 `2560x1600` 改成 `1792x1120`；
2. 断言加载器日志里出现按新窗口尺寸排布的面板矩形（`… window=1792x1120`）；
3. 断言插件在重排后上报的按钮绝对坐标**与之前不同**（`panel click sent 2152,334` →
   `panel click after resize sent 1384,261`），并且真实点击仍然命中
   （`Board panel: Ping clicked, count=2`）。

同一轮还顺手修掉一处布局浪费：内容子区域不再保留引擎自己那条（在本构建里不可交互的）
滚动条，插件拿到完整宽度（内容宽从 516 变为 530），宿主绘制的滚动条仍是唯一的滚动入口。

剩下只有一件需要人：**把窗口拖到另一台不同缩放的显示器上**再点一次。本机只有一台
显示器（`monitors=1`），脚本无法制造这个场景；`Display:` 那一行会在换显示器后给出新的
`window-dpi` 与 `ratio`，配合 `tests/ui-board-panel-playtest.ps1` 的点击断言就能判断
命中是否仍然正确。

## 中文输入与板上打字：人工验收（2026-09-18，用户确认）

`tests/manual-ime-test.ps1` 准备的一键沙箱（`build/manual-sandbox`，独立
`USERPROFILE/APPDATA`）里由用户实际操作完成，结论通过，并留下可核对的数据：

| 检查项 | 结果 |
|---|---|
| 页面里的中文输入（组合串、选词上屏、Esc 取消、中文标点） | 通过 |
| 电路板侧栏面板里用输入法打字 | 通过：缓冲区出现 `board panel buffer "asdasa啊啊倒莓的啊…"`（UTF-8） |
| 候选窗／组合窗定位 | **跟随光标**：`IME board panel composition=1970,259 candidate=1970,259 style=0x32`，随每个字依次 1990→2011→2032→2053→2073→2094→2250→2281→2301→2322，删字时退回 2301；`style=0x32 = CFS_POINT\|CFS_FORCE_POSITION` |
| 板上打字时的快捷键穿透 | 未发生：面板收到 `board panel saw key A`、`board panel saw key Space`，而画面里没有元件放置、仿真运行等板面反应（用户确认） |
| 观感（裁剪、滚动条、是否挡住游戏 UI） | 通过 |
| 已知现象 | 独占全屏下输入法候选窗出现时画面会**闪一下**（用户观察，见下方说明） |

**关于全屏闪烁**：候选窗是 Windows 自己画的独立窗口，不由加载器或插件绘制。独占全屏时
系统需要把它合成到游戏画面之上，OpenGL 交换链在这种切换中闪一帧是常见现象，加载器无法
从这一侧消除。可选做法（按代价从低到高）：改用无边框窗口化全屏；在输入法设置里关闭
候选窗的内嵌/动画选项；在 exe 兼容性里关掉"全屏优化"再试；若想知道到底是系统合成还是
游戏自己在切换窗口模式，可以在探针里加一段记录窗口样式/位置变化的诊断，复现一次即可判定。

## 键盘与字符输入验证（2026-09-18）

`tests/ui-keyboard-playtest.ps1`（先 `-Probe` 记录页面入口矩形，再跑驱动）在
`build/keyboard-sandbox` 里打开一个带 InputText 的插件页面，驱动
`tests/ui-keyboard-driver.hpp` 发**真实窗口消息**：带扫描码的 `WM_KEYDOWN/UP`、
`WM_CHAR`、`WM_IME_CHAR`。探针 `tests/ui-keyboard-probe.cpp` 把收到的键、焦点变化和
缓冲区内容写进日志，断言只看日志：

| 断言 | 证据 |
|---|---|
| 按键到达插件控件 | `Keyboard probe: key Tab pressed/down/up`、`key Escape pressed`、`key A pressed` |
| 真实按键打字不重不漏 | 只发按键消息（真实键盘的等价路径）：`buffer "" → "abc" length=3` |
| 单独的字符消息同样不重不漏 | 只发一条 `WM_CHAR`（z）：`buffer "abc\xe4\xbd\xa0\xe5\xa5\xbdz" length=10` |
| 非 ASCII 码元到达 | `buffer "abc\xe4\xbd\xa0\xe5\xa5\xbd" length=9`，即 `你`（WM_CHAR）与 `好`（WM_IME_CHAR）都进了缓冲区 |
| Escape 被输入框吃掉 | 之后是 `buffer ""`（ImGui 回滚文本），没有 `Closed UI page` |

同一条测试顺手纠正了一个**误判**（最早写在这份文档里的"字符会被重复投递"）：

| 发送内容 | 缓冲区结果 |
|---|---|
| 只发 `WM_KEYDOWN/UP`（真实键盘路径） | 1 个字符 |
| 只发 `WM_CHAR` | 1 个字符 |
| `WM_KEYDOWN/UP` + `WM_CHAR`（第一版驱动的错） | 2 个字符 |

原因是按键消息会被消息循环的 `TranslateMessage` 转成 `WM_CHAR`（与任何 Windows 程序
相同），再手工补一条就是两个字符。真实打字只有一条 `WM_CHAR`，因此**不需要插件去重**，
[sdk/ui.md](sdk/ui.md) 的能力表也按此更正。中文**组合串与候选窗**（preedit、
ImmSetCandidateWindow 定位）无法脚本化，留在同一份文档的人工验收清单里。
离线部分：`tests/ui-key.cpp` 检查 `keys::*` 的严格解析（缺导出即失败并报名字）、
未加载时是安全空操作、参数逐个转发，以及枚举值与实测一致。

## 电路板侧栏插槽验证（2026-09-18）

`tests/ui-board-panel-playtest.ps1` 在 `build/board-panel-sandbox` 独立副本里驱动真实
游戏（模式：`-Probe` 记录几何、默认驱动模式发真实点击、`-Example` 只验证发行示例）。
驱动代码 `tests/ui-board-panel-driver.hpp` 编进测试包 `dev.board-panel-driver`
（`build.ps1` 生成），发行示例 `example.board-panel` 不含测试代码。

这条测试一次跑完几件事，全部来自日志而不是推断：

| 断言 | 证据 |
|---|---|
| 注册与绘制 | `UI slot registered: main (board side panel)`、`Board panel dev.board-panel-driver/main frame=… x=1955 y=153 w=576 h=480 window=2560x1600` |
| 只在关卡里出现 | 面板第一帧日志出现在 `DRIVER: pressing a home page entry to reach a board` 之后；主菜单帧没有面板日志 |
| 真实点击到达插件 | `DRIVER: panel button hovered on frame …`、`DRIVER: panel button click reported count=1`、`Board panel: Ping clicked, count=1`；点击由 `WM_LBUTTONDOWN/UP` 加真实光标位置发出，目标点由面板矩形与按钮自身报告的位置算出 |
| 点击没有穿透到电路板 | 游戏自己的采样：面板点击窗口 `gate[panel] … active=1 bg=0 => busy=1`；同一测试在画布点做正对照 `gate[canvas] … active=0 => busy=0`，证明闸门不是恒为忙 |
| 裁剪 | 内容区下方 160px 处画一个探针控件，并按它"本应出现"的屏幕坐标发真实点击：拖动前 `clipped probe before scrolling hovers=0 clicks=0`，即被裁掉的项目既不可悬停也不可点击 |
| 面板边界 | 同一次点击落在面板矩形之外，游戏采样为 `busy=0` —— 面板只声明自己的矩形，面板外的点击仍归电路板 |
| 滚动 | 真实鼠标拖动宿主绘制的滚动条：`Host scroll strip … active=1` → 加载器直接读取的内容区滚动量 `Host content scroll … y=73 max=1268` |
| 折叠／展开 | 点标题栏开关：加载器记录 `h=60 … collapsed=1`（折叠帧仍显示旧矩形，所以断言的是一条标题栏高的新矩形）；折叠期间点按钮**不计数**（`count` 停在 1），展开后再点得到 `count=2`；折叠期间插件仍在画（`Board panel: draw count=…`） |
| 随场景关闭 | 驱动调用游戏自己的 `change_scene(ctx,0)`（ctx 来自加载器的 `TC_EVENT_SCENE_CHANGE` 负载，即游戏自己传给 `change_scene` 的那个对象），随后 `DRIVER: board stopped drawing after the scene change; panel frames=849`，之后不再有面板帧。这是硬断言：`scene.change` 改为加载器自持后这条曾退化成 NOTE，事件补上 `subject` 后恢复 |

实测日志（一次通过运行，节选）：

```
[dev.board-panel-driver] UI slot registered: main (board side panel)
[dev.board-panel-driver] DRIVER: panel first drawn on frame 122, content 550x412
Board input sample site reached; board panels are drawn from here
[dev.board-panel-driver] DRIVER: panel click sent 2147,335 tag=panel
[dev.board-panel-driver] DRIVER: gate[panel] frame=329 active=1 bg=0 hovered=0 => busy=1
[dev.board-panel-driver] DRIVER: panel button click reported count=1
[dev.board-panel-driver] DRIVER: canvas click sent 1280,700 tag=canvas
[dev.board-panel-driver] DRIVER: gate[canvas] frame=409 active=0 bg=-1 hovered=1 => busy=0
[dev.board-panel-driver] DRIVER: scrollbar drag sent 2488,231 +220 tag=scrollbar
Board panel scroll strip dev.board-panel-driver/main hovered=1 active=1 max=1268 scroll=0 x=2483 y=207 w=19 h=412
Board panel content scroll dev.board-panel-driver/main y=73 max=1268 contentEnd=489 childH=412
[dev.board-panel-driver] DRIVER: collapse toggle clicked at 2501,183 tag=collapse
Board panel dev.board-panel-driver/main frame=724 x=1955 y=153 w=576 h=60 … collapsed=1
[dev.board-panel-driver] DRIVER: button click while folded sent 2140,334 tag=folded (must not register)
[dev.board-panel-driver] DRIVER: expand toggle clicked at 2501,183 tag=expand
[dev.board-panel-driver] Board panel: Ping clicked, count=2
[dev.board-panel-driver] DRIVER: board stopped drawing after the scene change; panel frames=173
PASS board side panel: registered, drawn 173 frames on the board only, real click delivered …
```

离线部分：`build.ps1` 里的 `tests/ui-slot.cpp` 检查 `registerBoardPanel` 的宿主版本检查
（旧宿主按结构大小返回 −1，不越界读）、参数校验，以及交给宿主的定义内容。

两条测试基建教训记在这里，避免下次再踩：**沙箱必须每次刷新加载器**
（`make-ui-sandbox.ps1` 负责把 `dist/tc-loader.dll` 复制成 `game_engine.dll`，旧沙箱会
静默测到上一版加载器）；以及 **MinHook 钩子会跟进 `jmp` thunk**，此时被钩函数看到的
返回地址不再是被钩入口的调用点——加载器因此在直达路径之外增加了一次短线栈回溯。

第三件：**本构建的鼠标滚轮到不了 ImGui**。帖子窗口的 `WM_MOUSEWHEEL`（PostMessage 与
SendMessage 都试过）既没有改变内容区的滚动量，也没有改变其它可观测状态；内容子窗口本身
是可滚动的（`SetScrollY(90)` 读回 90、`ScrollMax=1268`），所以这不是子窗口的问题。因此
面板的滚动条由宿主自己绘制并驱动（拖动即滚动），滚轮相关的能力不做承诺。

## 顶部菜单栏插槽（2026-09-21）

**接口**：`TC_UI_SLOT_BOARD_MENU` + `tc::ui::registerMenuBarItem()`（见
[SDK 文档](sdk/ui.md)）。宿主在 `build_buttons__presenterZboard95uiZmenu95bar_u187`
清样式作用域时第一个**真正执行**的弹出点（`igPopStyleColor`，RVA `0x45a471`）画插件控件，
绘制发生在弹出之前，因此顶栏自己的按钮配色仍在作用域内。

**注入点是怎么选出来的**（第一版选错了，值得记下来）：

1. 先按"最后一个按钮的标签弹完字体"选了 `0x45a435`：控件注册成功、加载器日志也有
   `Top menu bar slot armed`，但**从不绘制**——`TC_MODLOADER_LOG_MENU=1` 探针显示该关卡里
   实际执行到的样式弹出是 `0x45a471`、`0x45a47b`、`0x45a485`（以及 `build_menu_bar` 自己的
   `0x45c014…`、`0x45ceea/0x45cef4`），`0x45a435` 属于可选的"返回关卡"按钮标签，普通关卡不执行；
2. 改用 `0x45a471` 后同一沙盒里立刻绘制。

**证据**（沙盒：`byte_adder` 关卡 + `example.byte-adder` 自动加载 + `example.board-panel`）：

```text
[example.board-panel] UI slot registered: bar (top menu bar)
[example.board-panel] Board panel: registered top menu bar item 'bar'
Board menu item example.board-panel/bar drawn in the game's top menu bar (frame 140)
```

截图 `build/menu-bar-out/bar7-top.png`：`Ping (bar) 0` 紧跟游戏自己的灯泡按钮，同一行、同一字号与
基线；`build/menu-bar-out/bar5-top.png` 是同一场景在修复注入点之前的对照（顶栏里没有任何插件控件）。

**回归**：`tests/ui-board-panel-playtest.ps1 -Example`（已把两条新插槽日志加进断言）通过；
`tests/ui-draw.cpp` 增加"按住不算点击"的断言后通过；fast 层 `io-value-service`、`sdk-abi`、
`compatibility-contract`、`punch-tape-layout` 全部通过。

## 波形面板：真机数据回放（2026-09-18）

`tests/waveform-playtest.ps1` 在 `build/waveform-sandbox` 独立副本里跑
`example.waveform-demo` 的驱动构建 `dev.waveform-demo-driver`（`build.ps1` 生成）。
驱动器 `tests/waveform-driver.hpp` 自己进入关卡：按主页入口的隐形按钮进入电路板、
从 `handle_update_wire` 拿 model、加载 `and_gate`、请求编译、把仿真跑到 cycle 6；
沙箱里预置了内置 AND 解（`build/and2_solution_builtin.data`）作为被测试的电路。
交接文档（现状、实测数据、未完成项与下一步做法）见
[research/waveform-handoff.md](research/waveform-handoff.md)。

断言全部来自日志与文件，不靠"看起来对"：

| 断言 | 证据 |
|---|---|
| 面板注册与绘制 | `UI slot registered: waveform (board side panel)`、`Waveform: registered board panel`、`Waveform: panel first drawn on frame …`；示例包 `hooks=0`，面板由加载器调度 |
| 槽位解析 | `Waveform: resolved slots in0@0 in1@8 (moved 2) out0@55 out1@64 (moved 2) (all found by movement)` —— 与离线单测里的实测布局一致；解析结果每变化一次就记一行，所以第一次"还没跑起来"的推断（`moved 0` + `stride estimated`）与之后按字节变化找到的最终结果都能看到 |
| 波形数据＝关卡自带测试 | 驱动器逐行上报面板要画的内容：`DRIVER: row … in=0,0 out=0,0` → `in=2,2 out=0,0` → `in=3,3 out=1,1`；脚本断言输出为高的行必定输入为 3、输入从 0 起、最大值 3、最后一行是 in=3/out=1 |
| 导出 VCD | `DRIVER: export rows=N path=…/waveform.vcd`；脚本读文件断言 4 个 `$var wire 64` 信号、有 `#3` 时间戳、`b11 i0` 与 `b1 o0` 都在 |
| 只在仿真时更新 | 驱动器记录行数 → 让游戏**暂停 2 秒** → 再记录：`DRIVER: paused rows=1` 与 `paused rows after 2s=1` 必须相等（暂停期间一行都不加）；开始运行后逐行断言 `cycle` **严格递增**，即一行对应一个新周期，而不是一帧一行 |
| 引脚数量与"重新运行清空" | 离线（`tests/trace.cpp`）：`setDeclaredCounts(1,1)` 后 `inputCount()/outputCount()` 变成 1/1；周期从 5 退回 −1 时行数清零、`takeRestartFlag()` 为真、随后从新的一轮重新记录。真机日志：`Waveform: the board has 2 input(s) and 2 output(s)`（板上 IO 元件数，替代不可信的 `level_used_input/outputs` 全局量） |
| 波形确实画在屏幕上 | 面板自己用 `glReadPixels` 抓帧写 `waveform.bmp`，脚本在**加载器报出的面板矩形**（`Board panel dev.waveform-demo-driver/waveform … x=1763 y=153 w=768 h=672`）内逐像素统计泳道颜色：一次通过运行得到输入蓝 `(90,170,235)`、输出黄 `(245,185,70)` 各数十像素（一个周期一行时波形很短，阈值只用来证明两条 lane 真的画出来了） |

实测日志（一次通过运行，节选）：

```
[dev.waveform-demo-driver] Waveform: resolved slots in0@0 in1@8 (moved 0) out0@0 out1@8 (moved 0) (stride estimated)
[dev.waveform-demo-driver] Waveform: resolved slots in0@0 in1@8 (moved 2) out0@55 out1@64 (moved 2) (all found by movement)
[dev.waveform-demo-driver] DRIVER: row 3 cycle=0 in=0,0 out=0,0
[dev.waveform-demo-driver] DRIVER: row 4 cycle=2 in=2,2 out=0,0
[dev.waveform-demo-driver] DRIVER: row 5 cycle=3 in=3,3 out=1,1
[dev.waveform-demo-driver] DRIVER: traced 4 rows, inputs=2 outputs=2, last row cycle=3 in=3,3 out=1,1
[dev.waveform-demo-driver] DRIVER: export rows=4 path=…\waveform.vcd
[dev.waveform-demo-driver] Waveform: exported …\waveform.bmp (framebuffer)
[dev.waveform-demo-driver] DRIVER: image …\waveform.bmp
PASS waveform panel: … added no rows during a two-second pause and one row per new cycle …
```

**这台机器上抓不到游戏窗口的像素**（重要，会影响后续所有"截图"类工作）：本构建全屏
运行在**独占翻转**的 GL 窗口上，`PrintWindow(hwnd, dc, 0)` 与 `PW_RENDERFULLCONTENT`
都返回整幅纯黑，`CopyFromScreen` 抓到的桌面里根本没有游戏窗口（窗口 `visible=True`、
`iconic=False`、`cloaked=0`，`GetForegroundWindow`/`SetWindowPos(HWND_TOPMOST)` 都不改变
这一点）。仓库里既有 UI 用例写出的 `menu.png`/`driver-page-open.png` 因此也是纯黑——
它们只能当"跑过"的凭证，不能当画面证据。要画面，只能由进程内自己 `glReadPixels`。

离线部分：`build.ps1` 编译并执行 `tests/trace.cpp`，按实测布局验证采样器的槽位发现、
数值读取、稀疏回退与 VCD 文本（`PASS simulation trace: slot discovery, values, sparse
fallback, VCD export`）。

### 内部节点（导线）探针：已实现并真机验证

面板可以监测**电路内部任意一条导线**：在板上选中导线 → **Probe wire**。链路是：

- 板模型：元件序列在 `model+0x78`（步长 `0x238`）、导线序列在 `model+0x98`（步长
  `0x68`）；`get_wire(model, point)` 返回的 id **就是导线在序列里的下标**（实测
  `id(a)=0,1,2` 与下标一一对应）。模型由 `load_level__modelZutilities_u7740` 的第一个参数
  捕获（这个 Hook 目前没有别的 Mod 使用；抢 `handle_update_wire` 会与 wire-palette 冲突）。
- 导线记录 `+0x38` 是它在仿真状态缓冲里的**字节偏移**、`+0x30` 是位宽；值等于
  `sim_state_read_u64(offset) & ((1<<width)-1)`，与游戏自己的 `sim_state_read_bits` 一致
  （反汇编核对：`read_u64` = `*(uint64*)(state_base + offset)`，`read_bits` = 它取低 width 位）。
- **采样时机**：仿真状态在步进结束时才写好。真机对照：暂停在 cycle 2 后读与运行中"周期刚
  变化"时读，同一个字节前者 0/1 正确、后者滞后一个周期。因此 `sample()` 把行延后一次调用
  落盘：捕获时读关卡 I/O，落盘时读探针。

真机断言（`tests/waveform-playtest.ps1`）：驱动器挑"接在关卡输出侧的那条导线"（端点 x 最大），
请面板加探针，然后逐行比较探针值与关卡自身的输出历史——`and_gate` 上两者都是 `0,0,0,1`，
一致；不一致就直接失败。

```
[dev.waveform-demo-driver] Waveform: driver probes wire 2 state@260 width=1
[dev.waveform-demo-driver] DRIVER: probe row=0 cycle=-1 value=0 out0=0
[dev.waveform-demo-driver] DRIVER: probe row=3 cycle=3 value=1 out0=1
Wire probe: driver probes wire 2 state@260 width=1 - value matched the level's own output on all 4 rows
```

未做：字宽网络的位宽字段核对（只用 1 位网络验证过）、元件引脚探针、编辑电路后探针失效的
重新解析。

## 自定义绘图回归（2026-09-18）

`build.ps1` 包含 `tests/ui-draw.cpp`：可选绘图能力缺失时的原子失败、重新加载失败后
清空旧绑定、局部坐标转换、输入状态快照、非法输入不提交、嵌套与异常退出裁剪配对。

`tests/ui-draw-playtest.ps1` 在 `build/draw-test-sandbox` 独立游戏／存档中加载真实引擎，
连续 180 帧验证所有绘图方法有顶点输出，矩形顶点位置／颜色与参数一致，窗口移动和
尺寸变化、子窗口滚动后的几何与裁剪仍正确，嵌套及画布裁剪都恢复原值。
测试专用探针只读顶点缓冲；SDK 不依赖该私有结构。

实测：`DRAW PASS frames=1/60/120/180` 全部通过。未把截图效果、真实拖动交互或多 DPI
支持计入这条自动化测试的结论。手工可启用 `example.drawing-demo.mod`，进入主菜单
Custom drawing 或按 F8，验证拖动控制点、松手计数、网格、线宽、重置与关闭重开。

## 沙箱机制

**别把 wire-palette 装进从现用安装复制出来的沙箱**：`make-ui-sandbox.ps1` 复制的是
`D:\p\asset`（里面已经是打过补丁的着色器），再 `apply local.wire-palette` 会**二次打补丁**，
结果 GLSL 编译失败、游戏启动即退出（进程退出码 1，连 `loader.log` 都不产生）。做界面截图
或临时沙箱时，Mod 列表里去掉 `local.wire-palette` 即可；需要它时就先在沙箱里还原
`asset/shader/*.vert`。

要还原就把现用安装 `state.json` 里该文件的 `original` 哈希对应的
`tc-modloader-data/blobs/<哈希>` 复制回沙箱的 `asset/shader/*.vert`，**再**执行 `apply`
（`tests/waveform-playtest.ps1` 之外的手工沙箱都按这个顺序来）。

## 板内界面怎么截图（开发用）

板侧栏面板、导线调色盘、电路只存在于关卡里，而菜单页的绘制只在主页运行，所以截图要用
`igEnd` 里的**定时抓帧** plus 一个只会“进入关卡”的小探针：

```powershell
$env:TC_MODLOADER_SHOT       = 'D:\shot\board.bmp'
$env:TC_MODLOADER_SHOT_DELAY = '14000'      # 毫秒：等 enter-board 进关卡、面板画出来
# 沙箱里 apply dev.enter-board,local.wire-palette 后启动游戏即可
```

`dev.enter-board`（`tests/enter-board.cpp`）只按一次主页入口，不 Hook 别的目标——板驱动
会因为占用 `handle_update_wire` 把调色盘挤掉（见上一条）。日志出现
`Captured frame screenshot` 后把 BMP 转 PNG 即可查看。

## 工具栏插槽（游戏工具栏里的 Mod 工具）

导线调色盘现在注册为**工具栏工具**（`TC_UI_SLOT_BOARD_TOOLBAR`），验证方式同上（沙箱 +
`dev.enter-board` + 定时抓帧）。一次通过运行的证据：

```
[local.wire-palette] UI slot registered: palette (board tool)
Board tool local.wire-palette/palette drawn in the game's tool column (frame 667, child 2560x932 at 0,718)
Board tools in the game's tool column (top to bottom): local.wire-palette/palette
```

截图可见色块按钮落在游戏工具列里、游戏自己的工具按钮正下方（`build/palette-shots/`）。
注入点是游戏最后一个工具按钮的 `igEndChild` 返回地址（`0x4659c1`，从实测的各子窗口几何
里挑出来的：工具按钮都是 x=366/462、y=174…666、80×80 的子窗口），绘制发生在该子窗口**结束
之后**，所以控件落在工具栏自己的窗口里；列的位置由加载器从这些按钮实测得到
（`noteToolColumn`），不是写死坐标。

### 展开界面里的文字（2026-09-18，已解决）

`tests/wire-palette-tool-playtest.ps1` 在 `build/wire-palette-tool-sandbox` 独立副本里跑真实
调色盘：驱动 `tests/toolbar-hover-driver.cpp`（`dev.toolbar-hover`）先进关卡，再把**真实鼠标**
压在插件瓦片上（本构建从真实光标读鼠标位置，所以只能 `SetCursorPos`），定时抓帧得到
`build/wire-palette-tool-out/open2.png`：展开面板里"导线调色盘/取色器/使用此颜色/新建颜色…"
等中英文都正常。这条用例同时打印：

```
Tool column text font: defined_fonts[2]=NoroshiCode_Regular.ttf (used for plugin tool draws)
Board tool local.wire-palette/palette drawn in the game's tool column (frame 346, child 2560x932 at 0,761)
Board tools in the game's tool column (top to bottom): local.wire-palette/palette
```

根因与证据（探针 `tests/toolbar-font-probe.cpp` + `tests/toolbar-font-playtest.ps1`，
反汇编见 `tools/scan-calls.js` 与 [research/toolbar-tool-handoff.md](research/toolbar-tool-handoff.md) §5）：

| 断言 | 证据 |
|---|---|
| 注入点当前字体是**图标字体**，不是字号问题 | `TOOLBARFONT tool ... font=Icon_Complete.ttf size=72`，同一插件在侧栏面板/主菜单页/帧尾都是 `NoroshiCode_Regular.ttf size=45` |
| 图标字体没有拉丁/中文字形，所以文字一个顶点都不产生 | 图标字体下矩形/按钮底可见、文字全无；推入正文字体后同一段文字立刻可见 |
| 游戏自己画工具栏标签的写法 | 反汇编 `build_function_icons__presenterZboard95uiZfunction95icons_u115`：`igPushFont(2)` → `igPushFontScale` → `igText` → `igPopFont()` → `igPopFontScale()`，注入点在这段作用域之外 |
| 宿主借字体后插件无需任何字体代码 | 探针里 `tc::ui::text(...)` 直接显示；调色盘端到端截图同样 |
| 宿主与游戏的字号一致 | 借用正文字体后 `igGetFontSize()` = 45，与侧栏面板相同 |

这条用例的沙箱准备比别处多一步：现用安装的 `asset/shader/*.vert` 已被本 Mod 打过补丁，
直接 `apply` 会二次打补丁、游戏启动即退出，所以脚本先按 `state.json` 里的 `original` 哈希从
`tc-modloader-data/blobs/` 还原这两个文件再 `apply`（顺序见本节「沙箱机制」）。

**关闭路径**同样观测到了：同一次运行里鼠标移开后日志出现
`Wire Palette: the expanded palette closed after the mouse left`，抓帧 `final-open.png` 就是收起
后的画面（瓦片还在、面板消失）。它不是一条"必然通过"的断言——沙箱与使用者共用同一个真实鼠标
与焦点，游戏窗口失焦后 ImGui 的鼠标位置不再更新，几何 hover 会一直为真；需要时可加
`-LeaveAfterMs 2000` 主动把鼠标移开再查日志。

### Mod 页面字号（2026-09-18，修掉一个静默失效的绑定）

页面容器原本按名字从**引擎 DLL** 取游戏的 `igPushFontScale` / `igPopFontScale` 想让标题与主页
同字号，但那两个是**可执行文件**里的 Nim 符号（引擎 1505 个导出里 0 个 `presenterZ…`），
所以一直解析失败、`if (withFont)` 永远为假，标题一直用窗口自带字号——不报错，也没人发现。

现在的做法：只对**宿主自己的页面窗口**调用引擎导出 `igSetWindowFontScale`（页头用主页倍率，
内容区恢复 1.0）。证据来自 `tests/ui-page-playtest.ps1 -DriverMode`（沙箱用
`make-ui-sandbox.ps1 -Mods dev.menu-demo-driver`，先 `-ProbeButtons` 记录入口）：

```
Mod page font scale applied: window font 45 -> 86
[dev.menu-demo-driver] Menu demo: page 'settings' first drawn on frame 124, content 2540x1497
PASS native UI page registered and drawn; screenshots in build/ui-page-sandbox/out
```

反面教训（记下来免得再踩）：把这两个游戏函数从 EXE 符号表绑上、真的去驱动游戏的字体栈，
会让页面**连绘制体都进不去**（`-DriverMode` 报 "The demo page never drew a frame"）。
宿主自己的窗口不需要游戏的栈。

纹理回归 `ui-texture-playtest.ps1` 使用 `build/texture-test-sandbox`。它实际回读 GPU
纹理的 RGBA 像素（包括透明度和上下方向），验证中文路径 PNG、损坏／缺失图片、
越界路径、非渲染线程、解包状态与纹理绑定恢复、UV 顶点、所有权、配额、重复释放、
拒绝初始化资源的延迟回收及连续 180 帧绘制。`ui-texture.cpp` 覆盖旧宿主降级、
移动所有权、重载失败保留原图、析构与显式释放。

每个真机用例都会：

1. 把游戏（EXE、引擎、`asset/`、`campaign/`、`translations/`）复制到 `build/<用例>-<guid>/game`。
2. 把 `USERPROFILE`/`APPDATA` 指向副本内的 `home`，因此存档完全隔离。
3. 用 `dist/tcmod-cli.exe <副本> apply <mod id>` 应用被测包。
4. 需要关卡时，把原理图写到游戏实际读取的那份电路：
   `profiles/default/schematics/<关卡 kind>/Default/circuit.data`（kind 见关卡自己的
   `campaign/<关卡目录>/meta.txt`；例如关卡 "The Sandbox" 的 kind 是 `architecture`），
   并按需写入用例开关（例如 `plugin-data/<mod id>/autotest.txt`）。
5. 启动游戏、等待、退出，然后**断言加载器日志、生成源码转储与关卡自身判定**。

失败时脚本会抛出带日志路径的错误；日志解读见 [reference/diagnostics.md](reference/diagnostics.md)。

## 原生逻辑回调场景（11 个）

| 场景 | 元件形状 | 关卡 | 游戏侧证据 | 独立复核 |
|---|---|---|---|---|
| `or` | 2 进 1 出（回调 OR） | `or_gate` | 判定 win，输出脚历史 `0,1,1,1` | 回调每周期恰一次 |
| `mixed` | 2 进 1 出 + 内置 NOT | `nor_gate` | 判定 win，输出脚历史 `1,0,0,0` | 两者都执行才可能通过 |
| `multi` | 同一定义 2 个实例 → 内置 AND | `or_gate` | 判定 win | 两实例各自每周期一次 |
| `shape1` | 1 进 1 出（NOT） | `not_gate` | 判定 win，历史 `1,0` | 输入元组 = 单脚 |
| `shape3` | 3 进 1 出（AND） | `and_gate_3` | 判定 win（8 周期） | 输入元组保序 |
| `shape32` | 3 进 2 出（全加器） | `full_adder` | 判定 win，同时校验 Sum/Carry | 43 样本复核 |
| `shapew8` | 8 位进 / 8 位出（×2） | `double_number` | 判定 win（16 周期随机字节） | UI 表格文本 |
| `shape_xor8` | 2×8 位 → 8 位 | `byte_xor` | 40 周期无失配 | 43 样本满足 `a ^ b` |
| `shape_mux8` | 3×8 位 → 8 位 | `byte_mux` | 40 周期无失配 | 43 样本满足 mux 语义 |
| `shape_asr8` | 8 位 + 3 位 → 8 位 | `byte_asr` | 40 周期无失配 | 43 样本满足算术右移 |
| `shape_adder8` | 1+8+8 → 8+1 位 | `byte_adder` | 关卡校验两个输出脚，40 周期无失配 | 43 样本满足 sum/carry |

另外每个场景都会断言：编译结果里关卡输入/输出元件数量与关卡定义一致、没有关卡输出脚
处于高阻；涉及重置的场景断言所有实例收到 `TC_LOGIC_RESET`。

### 为什么有的场景只断言“无失配”

`byte_*` 系列关卡的胜利条件需要 0xffff／0x1ffff 个周期（几万周期）。对这些场景：

- 自动验证脚本给 `autotest.txt` 写一个周期上限（例如 40）；
- 断言 `run finished cycle=40 ... verdict=0`，即关卡自己在 40 个周期内**没有**判定失败；
- 测试脚本再解析插件日志里回调交换的原始值，独立复核目标函数（43 个样本）。

两个方向都通过，才说明"进入回调的输入、回调算出的输出、写回关卡输出脚"这条链路正确。

## 0 脚元件：纯源与纯汇（2026-09-22）

证据与推理过程见 [research/custom-component-pins.md](research/custom-component-pins.md)；
这里只记"怎么验的"。

**离线层**（fast 层用例 `build` 里的 `tests/native-component.cpp`）：解码加载器生成的脚手架字节
（literal-only Snappy + v14 定义），逐节点断言形状——源 = `[0x12, 0x51]`（驱动门 + 输出脚，
没有收集门、没有依赖链），汇 = `[0x4f, 0x12, 0x12]`（输入脚 + 收集门 + 输出悬空的驱动门），
三输入汇 = 3+3+2+1 = 9 个节点；另外断言"两个方向都空"仍然被拒绝。

**真机层**（game 层用例 `pin-shape-source` / `pin-shape-sink`）：探针
`tests/pin-shape-probe.cpp` 复用 `example.byte-adder` 的关卡运行器，只把注册换成被测形状；
板子 fixture `nl_src0board.data`（源 + 内置 OR + 关卡 IO）与 `nl_sink0board.data`（汇 + 内置
NOT + 关卡 IO）由 `tests/and-component-fixture.cpp` 生成。断言三条：

1. `register_component` 返回 0，且日志有 `inputs=0 outputs=1` / `inputs=1 outputs=0` 与正确的
   引脚几何——游戏**导入并接受了 0 脚定义**；
2. `Native logic: bound instance … as token N`——编译后的实例内部门数与脚手架期望一致，
   也就是说**悬空的驱动门没有被编译器剪掉**（日志里的 `board layout` 行同时给出节点 kind：
   源是 `0x12`，汇是输入收集门和尾驱动门两个 `0x12`）；
3. 回调每拍一次：`cycle` 与 `calls` 同步递增（源 0→3、汇 0→1 的实测行写进了研究文档）。

因为 0 脚元件几乎不可能满足战役关卡的期望值（测试失败会让游戏停表、`verdict=2`），用例里由探针
**接管运行**（`simulation.run` 到指定周期）再数回调——这条对以后所有"元件行为"用例都适用。

## 实例、状态与生命周期（M2，2026-09-22）

**离线层**（fast 层 `build` 里的 `tests/native-component.cpp`）：手工建两个绑定并断言
`on_create` 各触发一次且写进的状态可见；`enumerate` 给出 2 个句柄、generation 不同、缓冲不够时
`ERR_RANGE` 且 `total` 正确；`info` 报出状态字数与脚数；`reset` 只影响目标实例（另一个的状态
不动、`resets` 计数不动）；伪造 generation 的句柄在 `validate`/`info`/`state`/`reset` 上一律
`ERR_STALE`；`releaseInstances` 释放"这次编译里没有"的实例并触发 `on_destroy`；释放后槽位被
复用时发放新 generation，旧句柄依然是 stale。

**真机层**（game 层用例 `pin-shape-wide9`，板子 `nl_wide9board.data` 上放**两个**同类型实例，
第二个不接线）：实测日志（原文见沙箱 `loader.log`）：

```text
pin-shape: wide9 on_create instance=2459565876494606882 phase=3 state_words=2
pin-shape: wide9 on_create instance=3689348814741910323 phase=3 state_words=2
pin-shape: instances enumerated: enum status=0 written=2 total=2 |
  #0 instance=0x2222222222222222 gen=1 info=0 calls=1 state_status=0 words=2 state0=1 |
  #1 instance=0x3333333333333333 gen=2 info=0 calls=1 state_status=0 words=2 state0=1
pin-shape: instances reset status=0 first_before=1 first_after=0 second_state=1
pin-shape: instances delete index=1
pin-shape: wide9 on_destroy instance=2459565876494606882 phase=4 state=3
Native logic: released 1 instance(s) (board recompiled)
pin-shape: instances after delete: enum status=0 written=1 total=1 |
  #0 instance=0x3333333333333333 gen=2 info=0 calls=8 state_status=0 words=2 state0=4
pin-shape: instances old_handle=-4 forged_handle=-4 (both -4 = stale)
```

覆盖了 M2 的验收：多实例计数器（两个实例各自递增状态）、创建（`on_create` ×2）、销毁
（删除元件 → `on_destroy` + 释放 + 枚举变 1）、重置（只影响目标）、旧句柄拒绝（真实旧句柄与
伪造 generation 都是 `-4`）。顺带确认了两件事：**未接线的实例照样被编译并绑定**（脚手架的
驱动门输入悬空，和纯源那次同一机制），以及**宽输出的 512 字节状态槽不再参与公共契约**
（实例状态在宿主内存里，`0x9a0000` 只留给宽输出回读这一个内部用途）。

实测踩到的三个坑也记在这里，免得下次重踩：

1. 关卡自己的运行在飞时，元件放置助手不肯提交（返回 false、棋盘元件数不变）——所以"两个实例"
   直接做进板子 fixture，而不是运行时放置；
2. `example.byte-adder` 的自动测试跑完后就不再更新自己的 `autotestElapsed`，探针的走查必须用
   自己的时钟；
3. 日志行是 CRLF，playtest 里 `...\d+$` 这类锚定会失配（`$` 不在 `\r` 之前）。

## 配置与仿真状态分离（M3 第一刀，2026-09-22）

**离线层**（`tests/native-component.cpp`）：V2 定义声明 schema 7、4 字节默认配置和两个状态字；
两个实例在 `on_create` 前已各自拿到默认配置。用例依次断言 count-only 读取、短缓冲拒绝、schema/长度
不匹配拒绝、整 blob 写入使 revision 从 1 变 2、下一次 RESET 回调读到新配置、RESET 清状态但不清
配置、状态快照恢复，以及伪造 generation 的存储访问返回 `ERR_STALE`。`tests/services.cpp` 另断言
`tc.component.storage` V1 的版本、表大小和五个入口。

**真机层**复用 `pin-shape-wide9`：九脚定义声明 schema 3 与默认配置首字节 7，实例走查通过服务把它
改为 42，再调用单实例 RESET 并回读。硬断言日志为：

```text
pin-shape: storage info=0 read_before=0 write=0 read_after=0 info_after=0 schema=3 bytes=4 before=7 after=42 revision=2
```

同一用例还继续断言九脚输入、两个状态字、多实例隔离、删除触发 `on_destroy` 与旧句柄失效，因而这条
证据来自真实游戏编译/绑定后的实例，不是离线伪对象。当前证据只覆盖活实例内存快照；保存→重启、
迁移、缺失 Mod 占位和 Undo 尚未实现，见 [HANDOFF-component-m3.md](HANDOFF-component-m3.md)。

## 配置写进元件存档（M3 第二刀，2026-09-22）

这一刀要证明的不是"文件里多了几个字节"，而是**配置真的经游戏自己的存档路径往返**。

**离线层**：`tests/component-tail.cpp`（fast 层，`build.ps1` 直接运行）覆盖记录编解码在
0/1/4/7/8/9/17/1024 字节上的往返、提交顺序（分块在前、校验和最后）、以及每一种拒绝理由
（缺记录、格式未知、换了定义、schema/长度不符、校验和不符、超过预算、表不可读）。同一文件还
证明不认识的表项原地保留。

`tests/native-component.cpp` 的 M3 段用一个测试自己拥有的替身表驱动宿主的存储路径：绑定即回读
（`on_create` 看到的是存档里的字节）、写入记录后内存与记录一致、**写入相同值不碰记录**、
记录写失败时内存配置保持旧值、以及"别的定义写的记录"既不被采用也不被覆盖。`tests/services.cpp`
与 ABI 快照同步新增 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE`。

**真机层**分两条独立证据：

1. `.\tests\component-persistence-playtest.ps1 -Mode insert`：探针**不再扫描哈希桶**，改用游戏
   自己的表赋值函数 `X5BX5Deq___modelZsave95mongerZversionsZv7_u70`（`save.custom_tail_set`）
   从夹具留下的**空表**开始插入，再灌入 48 个键迫使表扩容、存档、重启回读。三次运行输出：

```text
run 1: PASS insert inserted value=87109624524081870 count=49 slots=128
run 2: PASS insert readback 87109624524081870 then updated value=163971054138006495 count=49 slots=128
run 3: PASS insert readback 163971054138006495 stable value=163971054138006495 count=49 slots=128
```

   第 1 行里的 `slots=128` 就是扩容：空表第一次插入时游戏分配到 64 槽，活条目过 2/3 后翻倍。
   第 2、3 行的值只能来自第 1、2 次运行写下的 `circuit.data`，因为第 2 次启动的表内容是**游戏
   自己的反序列化代码**重建的（同一段的调用形状见
   `get_component__modelZsave95mongerZversionsZv7_u5+0x4cb`，它按 `(table*, key:i64, value:i64)`
   调同一个赋值函数）。

2. `.\tests\component-storage-playtest.ps1`：探针注册一个带 4 字节配置（schema 7）的一进一出
   元件，载入装有该实例的 `build\nl_not1board.data`，显式编译棋盘（未编译的关卡不会有绑定），
   然后**只通过 `tc.component.storage`** 读写配置并用 `save_this_schematic` 存档。三次运行输出：

```text
run 1: PASS storage launch=1 default value=11223344 stored=aabbccdd revision=1->2 persistence=1
run 2: PASS storage launch=2 readback-first value=aabbccdd stored=01234567 revision=1->2 persistence=1
run 3: PASS storage launch=3 readback-second value=01234567 stored=01234567 revision=1->1 persistence=1
```

   第 2 次启动读到的 `aabbccdd` 和第 3 次读到的 `01234567` 都只存在于第 1、2 次写下的原理图里；
   `persistence=1` 来自 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE`（宿主确实定位到了元件记录的表），
   日志按启动逐条断言：`Component storage: configuration persists in the component record's own
   table`、第一次启动的 `instance ... has no stored configuration`，以及后两次的
   `instance ... restored 4 configuration byte(s) from its saved record`（每次启动前清空
   loader.log，所以这些行只可能来自本次启动）。脚本同时断言目标 `circuit.data` 哈希相对初始夹具
   发生变化。

## 老存档的配置升级（M3 第三刀，2026-09-22）

**离线层**：

- `tests/component-tail.cpp` 断言"记录与定义不符时仍然读得出来"：`readStored()` 对 schema 6 /
  6 字节的记录返回 `Ok` 并给出它自己的 schema、长度与校验和，而 `decode()` 对同一条记录按当前
  定义分别报 `BadSchema`（长度相同）与 `BadLength`（长度不同）；来自别的定义的记录直接是
  `ForeignType`，不会进入迁移。
- `tests/native-component.cpp` 用替身表驱动三种结果：`MIGRATE_OK` 时回调**拿到的是老字节**
  （断言 schema、长度、`capacity`、`to_schema` 与字节逐一相符）、实例跑转换后的值、`on_create`
  看到的就是它、并且记录被升级成新 schema（把表重新解码一遍验证）；`MIGRATE_KEEP`、`REJECT`
  以及一个瞎返回的码则都保持默认配置、记录字节一个字节都没变（`table.values` 与写入计数都不动）；
  没声明迁移的定义不会调用回调，旧字节同样保留。迁移本身不算一次写入（revision 仍为 1）。

**真机层**：夹具 `build/nl_not1legacy.data` 与 `nl_not1board.data` 板型/接线完全相同，只是实例的
custom tail 里已经有一条 **schema 6、4 字节**的记录（`tests/and-component-fixture.cpp` 用与宿主
相同的 FNV-1a 算校验和）。探针注册 schema 7 与迁移回调，`TC_STORAGE_MIGRATE=accept|reject` 决定
回调返回什么。每次启动前清空 loader.log，因此每条断言只可能由本次启动产生。

```powershell
.\tests\component-storage-playtest.ps1 -Mode migrate
.\tests\component-storage-playtest.ps1 -Mode reject
```

```text
run 1: PASS storage launch=1 migrated value=a55a0ff0 migrations=1 legacy=1
run 2: PASS storage launch=2 upgraded-record value=a55a0ff0 migrations=0 legacy=0
（日志）instance ... migrated its stored configuration from schema 6 (4 bytes) to schema 7 (4 bytes)

run 1: PASS storage launch=1 refused value=11223344 migrations=1 legacy=1
run 2: PASS storage launch=2 refused-again value=11223344 migrations=1 legacy=1
（日志）instance ... refused to upgrade a stored configuration written under schema 6 (4 bytes)
```

第一组证明三件事：迁移拿到的是电路里的老字节（`legacy=1`）、转换结果成为活配置
（`value=a55a0ff0`，既不是默认值也不是老字节）、升级后的记录被存档并在下一次启动时**无需再次
迁移**地读回（`migrations=0`）。第二组证明相反的一面：拒绝升级后实例跑默认配置，而第二次启动
**又被喂了同一份老字节**（`migrations=1` 且 `legacy=1`）——记录没有被默认值覆盖，作者补一个
正确的迁移仍然来得及。`persist` 模式的输出同时加上了 `launch=` 标注，见上一节。

仍**未**证明、也仍未实现：缺失 Mod 占位、`on_clone` 与 Undo 快照。

## 缺失 Mod 时游戏到底丢什么（M3 第四刀测量，2026-09-22）

这一节先把问题**量出来**，因为"占位元件"该做什么完全取决于游戏自己怎么处理没有原型记录的
`0x4e` 元件。用例 `component-placeholder`（`tests/component-placeholder-playtest.ps1` +
不含任何注册的 `tests/component-placeholder-probe.cpp`）在四个场景里加载同一份夹具
`build/nl_not1legacy.data`（实例自带 schema 6 的配置记录），每次启动前清空 loader.log：

| 场景 | 实测结果 |
|---|---|
| 从未装过 Mod，只加载不存档 | 元件的槽变成 **kind 0 墓碑**（`components=3 custom=0 tombstones=1`），连线仍在且端点仍是原引脚坐标（`wire=0 a=(-6,-1) b=(-12,0)`），`circuit.data` **一个字节没动**（哈希等于夹具） |
| 在同一 profile 装回 Mod | `PASS storage launch=1 migrated value=a55a0ff0 migrations=1 legacy=1`——记录还在文件里，配置按第三刀的迁移路径回来了 |
| 缺 Mod 时存档 | 探针在墓碑状态存档，文件被改写；再装回 Mod 时 `FAIL storage probe found no instance`——**元件与它的配置记录已经从文件里消失** |
| 先装过 Mod（游戏已知原型）再移除 | 仍然是墓碑（`component=missing tombstones=1`），文件未被改写 |

结论（三条都要写进后续实现的前提）：**丢失发生在加载期，不在存档期**；**只加载不存档不会损坏
文件**；**一旦在缺 Mod 的状态下存档，配置就无法找回**。第四行还说明游戏不会跨会话保留自定义
原型——"玩家以前装过"并不构成保护。

测量顺带排除了一个想当然的修法：探针钩住 `get_custom_prototype__modelZboardZcustom95prototype95list_u451`
（`load_schematic_raw+0x855` 会调它）记录加载期间的每次原型查询，整次加载里**没有一次是针对
`0x4e4f54315f303031` 的**（只看到 `id=12dc0381356d9010 found=0` 两次，而该 id 并不在夹具里），
所以"给未知 id 返回一个占位原型"必须挂在**真正决定丢弃的那段代码**上，而不是这个查询函数。
夹具本体也核对过：解码 `nl_not1legacy.data` 后 `4e4f54315f303031` 在 payload 偏移 582、
`2222222222222222` 在 545，而 `12dc0381356d9010` 不存在。

**因此第 6 条仍未实现**：目前只有测量、可复现证据和"不要修在哪儿"的结论；下一步是反汇编
`load_schematic_raw` 里处理 kind `0x4e` 的那一段，找到它判定"原型不存在"的真实分支。

### 继续钉：丢弃点已经被夹到 `load_level` 内部（2026-09-22 同日）

同一套用例加了**场景 0 对照**：两个探针都启用（元件的 id 有人注册），夹具相同、dump 代码相同。

```text
scenario 0（带 Mod）: PASS placeholder component=present ... components=4 wires=2 custom=1 tombstones=1
  slot=0 kind=00 ... ; component=1 kind=4e x=-5 y=0 rot=0 id=2222222222222222 custom=4e4f54315f303031
    entries=5 54434d3300000001=..0001 54434d3300000002=4e4f54315f303031 54434d3300000003=0000000600000004 ...
scenario 1（缺 Mod）: PASS placeholder component=missing ... components=3 custom=0 tombstones=1
```

两条 dump 都在 **`load_level` 返回的同一时刻**，所以差别只有一个变量：那个 custom id 有没有注册。
另外注意场景 0 的记录仍是 **schema 6**——迁移发生在绑定（编译）阶段，符合第三刀的设计。

接着钩住 tail 表的写入函数（`X5BX5Deq___modelZsave95mongerZversionsZv7_u70`，解析器
`get_component__modelZsave95mongerZversionsZv13_u3+0x7ae` 自己就用它）得到：

```text
placeholder tail-set table=...5b60 key=54434d3300000001 value=0000000000000001
placeholder tail-set table=...5b60 key=54434d3300000002 value=4e4f54315f303031
placeholder tail-set table=...5b60 key=54434d3300000003 value=0000000600000004
placeholder tail-set table=...5b60 key=54434d3300000004 value=69c72aa8ba2c8e31
placeholder tail-set table=...5b60 key=54434d3300010000 value=000000003c2d1e0f
```

即**缺 Mod 时解析器照样把整条记录读出来了**（键值与我们写进去的逐字节相同），而且列表读取层
（`get_components__modelZsave95mongerZversionsZv13_u284+0xf8` 后）是无条件 `add`。所以丢弃发生在
**"解析完成 → 记录写进棋盘"之间**，且不是通过原型查询 API 决定的：

- `custom95prototype95list` 的 hashmap 全局 `cc_hashmap__..._u6` 只被本模块 5 个函数引用
  （`in_custom_prototypes` / `get_custom_prototype` / `custom_prototypes_set` / `custom_prototypes_del`），
  没有内联访问点；
- 这 5 个函数的全部调用者里，属于装载路径的只有 `load_schematic_raw+0x855`，而追踪显示它整次加载
  **没有一次带我们的 id**（两次调用的 id 每次运行都不同、且不在夹具里，属于噪声）；
- `recursive_customs__modelZboardZschematics_u6576` 是**文件系统**函数（`nosjoinPath` /
  `findFirstFile` / `files_get_bytes` / `parse_state`），负责递归读取设计依赖文件，不是元件过滤器。

下一步（窄到一次差分追踪）：同时钩 `update_uses__modelZboardZcustom95prototype_u780`、
`update_custom_used_components__modelZboardZcustom95prototype_u2729`、
`custom_prototypes_del__...u291`、`in_custom_prototypes__...u9`、`get_prototype__...u502`，
比较"带 Mod / 缺 Mod"两次加载里哪一个只在带 Mod 时被调用——那个就是要挂占位的地方。

### 捕获落点找到了：不需要再找丢弃那一行（2026-09-22 同日）

差分追踪之前先验证了一个更省的落点：解析器**把整条记录建在自己的栈上**——v13 反序列化器用
`[rsp+0xe0]` 存 kind、把 `[rsp+0x270]` 当 table 传给 `X5BX5Deq__...v7_u70`，两者相差正好
`0x190`（记录内表偏移）。于是钩住那个 setter 就能从 `table - 0x190` 读出**正在构造的整条记录**。
真机验证（`TC_PLACEHOLDER_TRACE=1`，缺 Mod 场景）：

```text
placeholder tail-set key=54434d3300000001 value=0000000000000001
  record=kind=4e x=-5 y=0 rot=0 id=2222222222222222 custom=4e4f54315f303031
placeholder tail-set key=54434d3300000003 value=0000000600000004
  record=kind=4e x=-5 y=0 rot=0 id=2222222222222222 custom=4e4f54315f303031
placeholder tail-set key=54434d3300010000 value=000000003c2d1e0f
  record=kind=4e x=-5 y=0 rot=0 id=2222222222222222 custom=4e4f54315f303031
```

即**在缺 Mod、元件注定被丢弃的情况下，宿主依然能拿到它的 kind、坐标、旋转、实例 id、custom id
以及全部 tail 键值**。据此在运行时实现了捕获与诊断（`src/native.hpp`）：

1. `armMissingModCapture()` 在启动时接管 `save.custom_tail_set`（先调原函数，再决定是否记账）；
2. `beginMissingModCapture()` 由 `level.load` 链节在**调用游戏原函数之前**打开窗口，于是窗口正好
   覆盖这次装载的解析阶段；
3. 捕获按实例 id 归档（每块板上唯一），只在记录 kind 为 `0x4e` 时记账；
4. 下一帧（`frame()` 里）`reportMissingMods()` 做判定：**该 custom id 没有注册定义**且
   **记录确实没落在棋盘上**才报告，并带上配置记录的 schema 与字节数。

真机日志（场景 1，缺 Mod）：

```text
Missing Mod: custom 0x4e4f54315f303031 at (-5,0) rotation 0 was dropped by the game because no Mod registered it; it carries a 4-byte schema-6 configuration record, kept for a reinstall
Missing Mod: 1 component(s) on this level have no owner; saving now would lose them
```

同一用例的**场景 0 对照（元件有主）没有这两行**，所以诊断不误报；场景 1/3/4 都断言有、场景 0 断言
没有。

### 救援：把记录放回棋盘（2026-09-22 同日）

诊断之后接上了两段：**落盘**与**注入**。

1. 缺 Mod 时把捕获的记录写进宿主自己的目录（`<loader 数据目录>/missing-mods/<关卡>.bin`，
   格式 `TCM3RSQ1` + 记录数 + 每条记录的 custom id / 实例 id / 坐标 / 旋转 / 全部 tail 键值）；
2. 下次装载该关卡时，如果某条记录的**主人已经注册**且棋盘的 (custom id, 坐标) 上没有对应元件，
   就用宿主已有的放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918`
   （与命令总线同一个入口、`board_edits::buildPlacement` 模板）把它放回去，再逐项把 tail 通过游戏
   自己的 setter 写回；成功后把该条从 store 里移除。

真机证据（场景 3 的两段日志）：

```text
（缺 Mod 且已存档）
Missing Mod: custom 0x4e4f54315f303031 at (-5,0) rotation 0 was dropped ...; it carries a 4-byte schema-6 configuration record, kept for a reinstall
Missing Mod: kept 1 record(s) for level not_gate in the loader's own data

（装回 Mod 再启动同一关卡）
Missing Mod rescue: put custom 0x4e4f54315f303031 back at (-5,0) rotation 0 with 5 stored configuration entries; the game assigns a fresh instance id, the wires reconnect by position
Missing Mod: kept 0 record(s) for level not_gate in the loader's own data
```

而这次启动里 `tc.component.storage` 的探针**找到了那个实例并完成迁移**：
`PASS storage launch=2 migrated value=a55a0ff0 migrations=1 legacy=1`——即救援放回去的记录仍然是
schema 6，走的是第三刀的正常升级路径。这一条取代了原先"缺 Mod 存档 → 装回也找不回来"的失败断言，
用例现在断言的是"能恢复"。

两点如实记录：

- **实例 id 会变**：元件由游戏的放置助手新建，因此拿到新的实例 id；连线按坐标连接，不受影响，
  但插件句柄（`tc.component.instances`）看到的是新实例。
- **救援是一次性的**：注入成功后条目即从 store 移除，所以之后玩家主动删掉它不会被反复"复活"；
  反过来说，如果玩家在缺 Mod 时存档、又在装回后删掉它，最早的那次救援仍会发生一次。

**仍未实现**：缺 Mod 期间**棋盘上可见的占位元件**。现在缺 Mod 时游戏依然把元件变成墓碑（我们只在
主人回来时把它放回去），所以"显示带诊断信息的占位元件"目前是日志诊断而不是画面上的方块；要做成
画面可见，需要 M5 的棋盘绘制（在记录坐标处画一个标记）或占位原型。

## Undo/Redo 入口侦察（M3 第五刀，进行中，2026-09-22）

M3 最后一项验收是"配置变更能进游戏原生撤销"。开工前先把游戏自己的撤销登记入口读清楚：

**入口**：`add_undo_changes__modelZboardZboard_u23805`（0x14014f950，实体在
`.part.0` 0x14014f3d0）。它的调用者正好是游戏的全部编辑操作——
`try_rotate_component`、`split_wire_at_point`、`try_delete`、`color_wire_point`、
`set_component_size`、`add_component`（组件菜单放置助手）、`set_wire_comment`、
`add_clipboard_to_board`、`delete`、`commit_drag_move`，共 10 处。

**参数形状**：一个参数（rcx），是一个 **Nim seq 头**——`[rcx]` 是元素个数、`[rcx+8]` 是数据指针，
元素步长 **0x490**（`lea rax,[r12+r12*8]` / `lea rbx,[r12+rax*8]` / `shl rbx,4` = 1168 字节），
函数把元素逐个复制到自己的栈缓冲（`rep stos` 计数 0x92 个 qword = 0x490）后按 **元素 +8 处的
tag 字节**分发（跳转表在 `0x140525918`）。

**它是变体对象，不是裸指针**：分发后的清理路径对同一个元素里十多个不同偏移逐一调用
`eqdestroy__modelZsave95mongerZversionsZv0_*` / `..._serialize_*`（每个 16 字节 = 一个 seq/string
头），说明这个"变更"记录里内嵌了完整的描述性数据（脚数、设置、引脚等），撤销栈保存的是**内容**
而不是内存地址——这正是配置（tail 表）能跟着走的理由。

**包装层还有一个提前返回**：函数开头按 `board[campaign[loaded_level]]`（`X5BX5D__modelZboardZboard_u23445`）
取到记录后检查 `[record+0x40]`，其值为 6、7 或 0 时**直接返回**（不登记撤销）。做测量时必须确认
我们用的关卡状态不落在这三类里，否则会得到"撤销没生效"的假结论。

**下一步（窄到一次测量）**：不继续靠反汇编猜字段，而是钩住 `add_undo_changes`，在
`component-placement` 那套已有的真机操作（放置/旋转/删除/尺寸）里把每个 tag 的元素前 0x60 字节
dump 出来——这样"组件变更"变体的布局（组件下标在哪、旧记录字节在哪）就是实测的。拿到布局后，
配置提交就照同样的形状登记一条，然后用 `board.undo` / `board.redo` 断言 tail 表回到提交前的字节。
要点：`src/native.hpp` 已经持有 `save.custom_tail_set` 的原始函数指针，撤销时由游戏自己回填表，
我们不需要自己实现撤销栈。

**第一次 dump 的结果（2026-09-22 同日）**：钩子已经装好（`tests/component-placement-probe.cpp`
的 `TC_UNDO_TRACE=1`，在放置/旋转/删除/改尺寸/事务那套真机操作里每个条目 dump 一次），
`component-placement` 用例照常通过，但**参数形状比"裸 seq 头"多一层**：

```text
undo-trace entry=1 arg=950670171600 count=1 data=2510000545856
  argbytes=0100000000000000 40e0b06748020000 4000000000000000 4010a16748020000 00… 4000000000000000 40d0af6748020000
  databytes=0100000000000000 00…（同一指针在四个条目里都相同）
```

即 `arg` 指向一个**多成员结构**（+0=1、+8=指针、+0x10=0x40、+0x18=指针、+0x30=0x40、+0x38=指针），
而不是单纯的 `{count, payload}`；`arg+8` 指向的缓冲以 `1` 开头、其余为零，说明真正的 0x490 元素在
**再下一层间接**上。因此下一刀不再从 wrapper 猜偏移，而是钩住它内部那个真正做元素复制的
`eqcopy__modelZboardZboard_u22985.part.0`（调用处 `rcx=dst` 栈缓冲、`rdx=src` 元素地址），
直接 dump **src** 的前 0x40 字节——那才是"变更记录"本身。dump 设施与钩子已经留在探针里，下一次
只需换目标函数。

### 第二个测量：两次 dump 都没拿到元素，改用"游戏自己撤销一次"来问（2026-09-22 同日）

钩 `eqcopy__modelZboardZboard_u22985.part.0` 后，四个撤销条目里 dump 到的 `src` 都是同一个地址、
内容全零；把 detour 换成两个参数后确认第二寄存器是 0，而第一个参数的头部是
`{1, ptr, 0x40, ptr, 0, 0, 0x40, ptr}`，三个缓冲的内容分别只有首字 `1`/`0x40`/`0x40`——说明元素在
**再下一层间接**上，从 wrapper 猜偏移这条路不划算。

于是换了一个**行为测量**（`tests/component-undo-probe.cpp` + `component-undo-playtest.ps1`，探针本身
如实报告、不当作门禁）：写配置 A → **选择并旋转**我们的元件（`select_component` + `try_rotate_component`，
旋转是已知会登记撤销的编辑）→ 写配置 B → 调 `board.undo` → 读配置。结果是：

```text
undo: record=2394377384576 wrote=a1a2a3a4 afterRotate=a1a2a3a4 wrote2=b1b2b3b4 undo=0 afterUndo=b1b2b3b4
```

即这次撤销**没有生效**（`undo=0`）。同一次尝试还暴露了两个必须记下来的坑：

1. **`board_delete_component__modelZboardZboard_u10711` 不登记撤销**：先用它做删除再撤销，
   元件没有回来——它是底层删除，UI 的删除（`delete__presenterZutilitiesZhelper95functions_u5932`）
   才调 `add_undo_changes`。选错入口会得到"撤销没用"的假结论。
2. **不能通过 `tc.component.storage` 读"撤销后"的配置**：删除元件后绑定仍然存在（要到下一次编译
   才释放），服务的读拿到的是宿主内存里的旧副本（那次读到 `a1a2a3a4`，而棋盘上元件已经没了）。
   判断撤销是否恢复记录，必须**直接读棋盘记录里的 tail 表**。

### 结果：宿主侧的配置事务 + 接管撤销入口（2026-09-22 同日）

第三次尝试把上面两条都用上，并得到一个结论**和一个实现**：

- 结论：**游戏自己的撤销栈不表达配置变更**。它的变更种类全是棋盘编辑；我们试着用"选择 + 旋转"
  登记一条关于该元件的条目，在这个人工上下文里旋转被拒（接线会撞线，新放的未接线实例也一样），
  而底层删除不登记撤销——两条路都不通。于是不再依赖游戏表达配置变更。
- 实现：宿主自己保存一步的**前后字节**（`src/native_logic.hpp` 的 `ConfigEdit` / `configUndoStack`
  / `configRedoStack`），并在 `src/native.hpp` 里接管 `board.undo` / `board.redo`：只要还有未消费的
  配置步骤，就把它写回元件记录并报告"已处理"，**栈空时原样调用游戏的撤销/重做**。上限 64 步，
  LIFO，新写入清空重做栈，实例释放或换板丢弃相关步骤。

真机证据（新用例 `component-undo`，15.4s 通过；撤销与重做都走**游戏自己的入口**——命令总线，
玩家 Ctrl+Z 调的是同一个函数；每次读**棋盘记录里的表**，不是服务）：

```text
PASS one undo restored the first configuration and one redo re-applied the second:
     instance=2459565876494606882 wrote=a1a2a3a4 then=b1b2b3b4 afterUndo=a1a2a3a4 afterRedo=b1b2b3b4
```

顺带钉死的一个调用约定（之前的失败全来自这里）：**游戏 Board API 的第一个参数是 `load_level` 接收
的那个对象**，`+0x78` 只是它内部的元件表；把 `+0x78` 当 board 传，放置和撤销都会静默失败。

**仍未做**：把一次配置提交与棋盘编辑合并成同一组撤销（`tc.transactions` 目前只暂存命令）。

## M2 回补：`on_config_changed`（2026-09-22）

**离线层**（`tests/native-component.cpp`）：定义声明 `on_config_changed` 后，

- 一次成功的 `write_config` 触发**恰好一次**，回调里 `io->config[0]` 就是新字节；
- 再写入同样的字节**不触发**；
- **在回调里调用 `storageInfo()` 返回 OK**（这条是锁纪律的回归测试：第一版把通知留在实例锁内，
  这条断言会直接死锁——实测确实挂住了，改成"锁内改状态、锁外通知"才通过）；
- 一次 `undoConfigEdit()` 触发一次并看到被恢复的字节，`redoConfigEdit()` 同理，且记录里的表与
  内存里的配置一致（用 `component_tail::decode` 复核）。

**真机层**（`component-storage` 三个模式）：探针的定义声明 `on_config_changed` 并在 PASS 行里报告
`config_changes=`，playtest 逐个断言：

```text
persist  : launch 1 -> config_changes=1（写了一次）
           launch 2 -> config_changes=1（写了一次；装载存档记录不算改变）
           launch 3 -> config_changes=0（没有写入）
migrate  : launch 1/2 -> config_changes=0（迁移与读回都不是"改变"）
reject   : launch 1/2 -> config_changes=0
```

三个模式全部通过（36.9s / 25.7s / 26.1s），说明"写入算一次改变、装载与迁移不算"这条语义在真机上
也能被插件观察到。ABI 基线新增 `TC_LOGIC_CONFIG_CHANGED` 与
`offsetof(TCComponentLifecycleV1, on_config_changed)`（结构 24 → 32 字节，尾部追加）。

**M2 回补剩余**：`on_clone`、`on_load/on_save`，最后是 move/rotate/resize 与编译类回调。

## V2 定义：九脚元件（`tc.component.types`，2026-09-22）

**离线层**：`tests/native-component.cpp` 断言 9 脚与 16 脚脚手架的节点数（`9+9+8+1+1`、
`16+16+15+1+1`）、V1 结构体对 9 脚仍然拒绝、以及 129 位输入的预算拒绝（128 位正好通过，
单脚 65 位被拒）。

**真机层**（game 层用例 `pin-shape-wide9`）：探针用 `tc::component_types::registerDefinition`
注册一个 9 输入 1 输出、`state_words=2` 的定义，板子 `nl_wide9board.data` 把 9 根输入线从关卡的
同一米源扇出（一根线一个脚），输出接关卡输出。断言三条：

1. `pin-shape: wide9 V2 registration (9in/1out) -> 0 (ok)`，日志同时给出
   `shape=9in/1out v2 state=2` 与 9 个引脚的相对坐标（`in0=(-2,-4,w1)` …）；
2. 编译后的棋盘 `board layout` 里，这个实例挂着 **9 个 `0x50` 脚 + 9 个 `0x12` 收集门 +
   8 个 `0x17` 依赖门 + 1 个 `0x12` 驱动门**，并且 `bound instance … as token 1`；
3. 回调每拍一次，日志逐拍打印 9 个输入与状态字：
   `wide9 cycle=3 inputs=9[1,1,1,1,1,1,1,1,1] state=3 calls=4`——九脚都拿到了值，
   `state` 按定义的两个字自增（V1 的固定八字不再决定状态大小）。

板子 fixture 里一个格式坑也记在这里：`0x4000|0` / `0xC000|0` 是**路径结束**（长度 0），
所以"接在同一条水平线上的那个脚"必须整个跳过竖直段，否则那根线只走到一半、脚读 0。

## WordWatchee 64 位数值标签（2026-09-20）

包：`local.word-watchee-64`（`format: 2`，原生插件 + 两个着色器文件）。定位过程、反汇编证据
与双排布局的取舍见 [research/word-watchee-64.md](research/word-watchee-64.md)。

**离线层** `node tests/word-watchee-model.js`（fast 层用例 `word-watchee-model`）：脚本不接受
任何"插件自述"，它读的是**钉住的 `Turing Complete.exe`** 与**包里的两个着色器**：

1. 按 COFF 符号 `set_value_size__…word95watchee95mesh_u1008` 求地址，读函数 +0x40 起 24 字节，
   与 `examples/word-watchee-64/plugin.cpp` 里的 `kClampBytes` 逐字节比对——插件里的表和真实
   二进制对不上就在这里失败；
2. 断言这 24 字节确实是 `mov eax,0x20` / `cmp bl,al` / `cmova ebx,eax`，并模拟 `min(size, ceiling)`：
   未打补丁时 64 → 32，打补丁后 64 → 64，而 32/8 位不变；
3. 从包的 `.vert` 解析 `ROW_STRIDE`，断言两个着色器步长一致、都声明两行字形数组、片元用
   `int(row) * ROW_STRIDE + int(whole)` 按行寻址、`shift = 28` 在位、宽分支挂在
   `inst_value_size > 32u` 上，且该分支里**没有** `u_format`／`OVERRIDE_FORCE_SIGNED`
   （宽标签不跟随显示格式），较短行由 `pad_row()` 补空格；
4. 按 `repr_wide()` 的语义对宽度 33–64 取样：十进制数字在第 10 位处换行，**用户给的例子**
   `0x8000000000000000`（2^63）必须得到上排 `9223372036`／下排 `854775808`；
   `0xffffffffffffffff` → `1844674407`／`3709551615`；40 位 `0xffffffffff` →
   `1099511627`／`775`；不足 10 位（如 33 位 `0x1ffffffff`）→ 上排 `8589934591`／下排 `0`。
   另有覆盖 33–64 位 × 5 个取值的独立不变量：两行拼回去必须等于该值的十进制字符串，
   且每行不超过 10 格（160 组）。
5. 断言 ≤32 位单行路径仍在（`repr16(inst_value)`、`repr10(inst_value, u_format >> 2 < 0)`），
   并按十六进制／无符号／有符号取样：`deadbeef`、`3735928559`、`-1`、8 位 `-128`、4 位 `-6`。

**真机层** `tests/word-watchee-playtest.ps1`（game 层用例 `word-watchee`）：

1. 隔离沙箱安装 `dist\tc-loader.dll` 与 `dist\local.word-watchee-64.mod`，用 CLI 应用；
2. 解压包、把两个着色器与沙箱里实际部署的文件**逐字节比对**（SHA-256），再断言 `.vert` 含
   `shift = 28;` 与 `label_rows = 2.0;`、`.frag` 含按行寻址，`state.json` 记录两个文件的前后
   哈希且原文件进了 `blobs/`——"修复在沙箱里"与"游戏读的就是包里的文件"因此是同一句话；
3. 启动游戏：插件把真实函数里的上限字节从 `0x20` 写成 `0x40`，把读回的字节写进
   `plugin-data/local.word-watchee-64/result.txt`（`before=mov eax,0x20 after=0x40 ceiling=64`），
   日志同时给出窗口地址（实测 `0x…80f0`，与离线推导的 RVA 一致）；
4. 同一沙箱里再装一个一次性探针（`tests/enter-board.cpp` 编译的 `test.enter-board`）把游戏从
   首页带进棋盘：截图里是 Sandbox 电路板，日志有 `ENTER-BOARD: pressed a home page entry`，
   且没有 `Native failed:`／`Failed to create shader`／fault 记录——证明打了内存补丁的引擎
   仍然正常绘制电路；
5. `-Loader <path>` 可以把加载器换成"玩家已装的那一份"来复验：本机 `D:\p\game_engine.dll`
   （0.4.0 加载器）实测同样通过（该加载器没有 `report_status`，所以 Mods 页状态行改成按
   日志版本条件断言，文件与日志证据不变）。

**画面层** `tests/word-watchee-layout-playtest.ps1`（diagnostic 层
`word-watchee-layout-diagnostic`）：真实 Byte Adder 关卡 + 夹具板，再装一次性探针
`tests/word-watchee-wide-probe.cpp`：它把位宽强制成 64，并把标签数值强制成 `-Value` 指定的
数（默认 2^64-1），于是普通 8 位棋盘的标签也走宽标签路径、而且数字已知。截图核对：

| 强制值 | 期望（上排／下排） | 实测 |
|---|---|---|
| `0xFFFFFFFFFFFFFFFF` | `1844674407` / `3709551615` | ✓ |
| `0x8000000000000000`（2^63） | `9223372036` / `854775808` | ✓ |

这两个值就是 §7.1／§7.2 两个真机 bug 的复现样本：第二行曾只剩第一位（驱动把动态下标的
局部数组编译坏了），2^63 曾因游戏自带 `divmod10` 差一而少 1。小值（如 `Carry in` 的 `1`
或 `A` 的 `230`）按规则显示成上排数字、下排 `0`。注意这是**探针强制**的结果：真实游戏里
只有位宽 >32 位的标签才双排，1/8/32 位标签仍是一行。

**仍未验证**：一块真正的 64 位棋盘上的完整数值——仓库里的关卡与夹具只有 1/8/32 位字长，
需要先有 64 位自定义元件或字长编辑器产出的板子。想现场核对时可用 `TC_WATCHEE_TRACE=1`
启动：插件会钩住 `set_value_size` 并打印前 64 次调用的 `size`。

## 其他真机用例

| 用例 | 覆盖 |
|---|---|
| `and-component-playtest.ps1` | 电路定义导入：64 位 ID、引脚数量与位宽、名称、快照释放 |
| `component-placement-playtest.ps1` | 组件菜单使用的放置 helper 把 `0x4e` 实例落到 board |
| `component-persistence-playtest.ps1` | 保存电路重启两次后 ID 与导线保持 |
| `component-timing-playtest.ps1` | 声明统计进入编译统计与界面分数（单件/串联/并联/0/内置/嵌套/沙盒/工坊/多驱动，9 例） |
| `byte-adder-smoke.ps1` | `example.byte-adder` 真实包：引脚几何、刷新阶段与周期阶段的加法取样、关卡 40 周期无失配 |
| `native-component-playtest.ps1` | 声明式 8 输入／8 输出：重复输入次序、最后两个输出接关卡、重复 ID／非法描述拒绝、40 周期无失配 |
| `native-component-playtest.ps1 -Single` | 声明式 1 输入／1 输出字节元件：`double_number` 16 周期全部通过 |
| `kind-list-playtest.ps1` | 枚举 125 个内置元件的 kind／名称／引脚（`build/kinds.txt`） |
| `kind-list-playtest.ps1`（引脚封装回归） | 探针改用 `tc::prototypeInput/OutputPinPoint()` 与 `...PinWordSize()` 后，真机重新 dump 的 126 行与改动前逐行一致：名字／引脚数／偏移／字宽全同，证明 SDK 隐藏的 `TCPin* + 8` 锚点算术与手写实现等价（[research/board-object-fields.md](research/board-object-fields.md) §2） |
| `ui-playtest.ps1` | 界面封装：真实 `mod-inspector` 包的面板在 ImGui 帧内绘制，视口/鼠标返回值合理 |
| `waveform-playtest.ps1` | 关卡波形：驱动器进入 `and_gate` 并运行，断言采样行＝关卡自带测试、VCD 内容、以及面板矩形内的泳道颜色像素（`-Example` 只验证发行包注册） |
| `word-watchee-playtest.ps1` | `local.word-watchee-64`：两个着色器与包内副本逐字节相同、`state.json` 的前后哈希与备份、插件在真机内存里读回的位宽上限字节、进入 Sandbox 棋盘截图（可用 `-Loader` 复验旧加载器） |
| `word-watchee-layout-playtest.ps1` | 双排宽标签的画面证据：Byte Adder 棋盘上把位宽强制成 64，截出上排低 32 位／下排高 32 位（diagnostic 层） |
| `sim-state-probe`（开发探针） | 仿真状态映射与整板解释器实验（非发布路径） |

## 断言来源原则

1. 优先使用游戏自身产生的证据：关卡判定结果、关卡输出脚历史、UI 表格文本、编译统计。
2. 插件日志只用于补充过程事实（调用次数、交换的数值），并尽量在测试脚本里**独立重算**
   目标函数，避免"插件自己说自己对"。
3. 不把开发期探针（`dev.*` 包、`TC_GUARD_SELFTEST` 等）算作玩家功能验证。

## 作者自检清单

- [ ] 包能应用并重启后正常加载，日志无拒绝行。
- [ ] 自定义元件能在 Sandbox/Foundry 放置，关卡测试判定通过（或按本页说明用有界断言）。
- [ ] 元件行为由回调决定（内部电路是占位门），且关卡判定、暂停、重置仍由游戏执行。
- [ ] 多实例场景下每个实例状态独立。
- [ ] 日志中没有 `registration rejected`、`exposes N operand(s)`、`has no recognised internal logic node`。
- [ ] 字宽元件确认输出脚宽度与定义一致，且 16/32 位等未验收组合被明确标注。

## 未验证边界

- 16/32 位字宽关卡（战役里只有 symphony 系列有 32 位 IO，其判定依赖 RAM 与指令执行，不适合作为元件用例）。
- 字宽状态保持（计数器类元件）、暂停语义、3 个以上实例组合。
- 元件内部中间层（按计划暂缓，留待后续独立 Mod）、RAM/寄存器作为逻辑源。
- 实际鼠标拖动、战役通关、不同显卡/窗口配置、多第三方原生插件组合。
- 波形采样只覆盖当前关卡的普通 I/O 历史：字宽关卡不一定写输出历史（这类关卡输出波形
  可能为空），`and_gate` 之外的关卡（引脚更多、输入不连续变化）尚未逐个验收。
- 安全模式已实现但未做物理按键实机验证。
- Steam 云服务联网行为、游戏更新后的重新适配。

# 主菜单页面与游戏外观按钮：验收

对应改动见 [research/native-ui-pages.md](research/native-ui-pages.md) 与
[sdk/ui.md](sdk/ui.md)。下面区分"自动测试证明的"和"需要人看一眼的"。

## 自动测试证明的

```powershell
./build.ps1                                   # 全量构建 + 既有全部单测
./tests/make-ui-sandbox.ps1 -Mods dev.menu-demo,dev.menu-demo-peer
./tests/ui-page-playtest.ps1 -ProbeButtons    # 记录主页按钮矩形
./tests/ui-page-playtest.ps1 -DriverMode      # 打开页面、绘制、输入、容器尺寸
./tests/ui-page-playtest.ps1                  # 两个插件注册同名页面
```

`-DriverMode` 断言的证据（真机运行的真实日志）：

```
DRIVER: container window size 2560x1600
DRIVER: Apply item size 111x51
DRIVER: full-region probe itemHovered=1 windowHovered=1
DRIVER: CONFIRMED - a click reached a widget inside the page container
```

即：容器按视口铺满、控件能收到 hover、点击能到达控件。另外断言两个插件各自注册了同名
页面、页面确实绘制过、`on_frame` 与页面各自计数互不抢帧、没有回调异常。

## 人工验收记录（2026-09-18）

```powershell
./tests/ui-page-demo.ps1 -Peer
```

隔离副本、两个插件包（`dev.menu-demo` 与 `dev.menu-demo-peer`，**相同页面 ID、相同控件
标签**）同时加载，逐项确认：

| 检查项 | 结果 |
|---|---|
| 主菜单出现两个同名页面入口 | 通过 |
| 点入口打开整屏页面（替换主菜单，不是浮层） | 通过 |
| 页面内 `Apply` 可点击、计数递增、悬停变色 | 通过 |
| 返回按钮回到主菜单 | 通过（返回后又打开了另一个插件的页面） |
| 另一个插件的同名页面独立工作、计数互不影响 | 通过 |
| 进关卡后页面不残留 | 通过 |
| 外观观感 | 接受当前状态，**不要求**与游戏原生 UI 完全一致 |

对应日志：

```
[dev.menu-demo] UI page registered: settings
[dev.menu-demo-peer] UI page registered: settings
Opened UI page dev.menu-demo/settings
[dev.menu-demo] Menu demo: page 'settings' first drawn on frame 5017, content 2560x1497
[dev.menu-demo] Menu demo: Apply clicked on settings
Opened UI page dev.menu-demo-peer/settings
```

`Apply clicked` 那一行是人工点击产生的：自动化一直没能稳定复现它，因为合成光标会被
这台机器上其他程序的光标锁定覆盖（细节见
[sdk/ui.md](sdk/ui.md#容器内输入已验证)）。所以真实的鼠标点击这一项由人确认。

## 已知限制（设计如此，非缺陷）

- 页面容器与板侧栏插槽现在共用同一段裁剪／滚动实现（`src/native.hpp`
  `drawContentRegion`），内容超过容器高度时滚动而不是画到区域外；页面容器里的
  滚动行为由插槽用例的真机拖动断言覆盖（同一条代码路径），页面侧只断言内容区
  与滚动条存在（`UI page content …`、`Host scroll strip …`）。
- 页面容器内不能画自定义图形（矩形、线段、图片）：`ImDrawList_*` 从插件调用会卡死
  渲染线程。
- 游戏外观按钮没有点击音效，悬停是瞬时换色（主菜单自己带过渡动画）。
- 不能热卸载、不能运行时增删页面，只能重启。
- 取光标在窗口内的位置、控件矩形范围：`igGetCursorPos`、`igGetWindowPos`、
  `igGetItemRectMin/Max` 在本构建返回 `0x80000000`，SDK 刻意不暴露。
- 多显示器（把窗口拖到另一台不同缩放的显示器）未测；本机 175% 缩放桌面上的几何与点击
  已经跑通，见上面的显示小节。

## 还没做的

- 键盘按键、字符输入与中文输入法已实测（自动 + 人工各一轮）；窗口尺寸变化后的几何与
  点击已自动断言，175% 缩放的桌面也已覆盖。剩下的只有"换到另一台缩放的显示器"。
- 电路板侧栏插槽目前只有一个位置（右侧）一种形态：没有折叠／隐藏按钮，也没有和游戏
  自身工具栏的位置协商；面板被宿主缩放到窗口内。
- 侧栏插槽自身没有做键盘导航与 IME 测试，也没有做多显示器／DPI 变化回归。
## M3 收口：事务分组与存档上限（2026-09-22）

**编辑事务分组**（计划 §9.3 要求批量工具显式说明动作的起止）：`tc.component.storage` V2 追加
`begin_edit` / `commit_edit` / `abort_edit`。真机 `component-undo` 现在跑两条路径：

```text
两次单笔写入 -> 一次撤销回到第一笔、一次重做回到第二笔
begin_edit -> 写第三笔 -> 写第四笔 -> commit_edit -> 一次撤销回到 begin 时的字节、redo 回到组末态
```

即**一次撤销对应一次玩家动作**，而不是组内每一步各占一条。离线并覆盖重复 `begin`/`commit`/`abort`
返回 `ERR_STATE`，以及"回退必须同时改记录与宿主内存"——`component-undo` 早先只改记录，服务会读到
旧值（同一类陷阱第二次出现，已修并写进用例）。

**1024 字节上限**（`component-capacity`，两次启动）：

```text
run 1: PASS capacity written launch=1 entries=5 file=235 wrote=identical entriesAfterWrite=132
run 2: PASS capacity survived save plus restart: launch=2 entries=132 file=1203 readback=identical
```

- 满配实例恰好占 **132 个表项**（4 头部 + 128 分块），跨重启字节一致；
- 原理图 235 → 1203 字节，即**每个满配实例约 1 KB**（原始 132×16 ≈ 2.1 KB，Snappy 压缩后）；
- 顺带记一个现象：游戏在装载关卡时会把存档重写成规范化形式（夹具 752 → 235 字节），所以文件大小
  要在装载之后再读才有意义；
- 由此把上限的取舍写清楚：1024 字节在容量与代价上都站得住（单实例约 1 KB 存档），但如果将来要放大
  上限，应按这条链路的实测方法重测，而不是直接引用内存层的 64 KiB。

**M3 到此收口**：存储契约、迁移、缺失 Mod、编辑事务四条验收全部有离线 + 真机证据。留待后续的三项
（画面占位 → M5、仿真状态入档 → §18 决策、`on_clone`/`on_load`/`on_save` → M2 回补）都写进了
[HANDOFF-component-m3.md](HANDOFF-component-m3.md) 的收口清单。

## M2 回补：`on_clone` 与复制语义（2026-09-22）

复制在游戏里走的是 UI presenter 上下文（`copy_selection_to_clipboard` / `add_clipboard_to_board` 都是
0xd2xx 栈帧的界面助手），探针驱动不了；因此把"复制"定义成**宿主自己的编辑操作**：命令总线新增
`TC_COMMAND_BOARD_DUPLICATE_COMPONENT`（`argument` = 源实例 id，`custom_prototype_id` = 类型，
`x`/`y`/`rotation` = 目的地）。

**离线层**（`tests/native-component.cpp`）：普通绑定不触发 `on_clone`；被标记为副本的实例触发一次，
`on_create` 与 `on_clone` 看到的是抄来的字节（0x77…），副本的配置与源一致，标记用后即清。

**真机层**（`component-undo` 扩展，连续两次运行约 15s 通过）：

```text
Component clone: carried 4 configuration byte(s), schema 7
Component clone: duplicated custom 0x4e4f54315f303031 from instance 0x2222222222222222 to (0,6) with 5 configuration entries
undo probe: PASS one undo reverted a grouped configuration edit, one redo re-applied it,
            and a duplicated instance arrived with the source's configuration and announced itself:
            instance=2459565876494606882 wrote=a1a2a3a4 then=b1b2b3b4 grouped=d1d2d3d4 clone=d1d2d3d4
```

即：副本记录带着源配置（`clone=d1d2d3d4`）、它的第一次绑定发出 `TC_LOGIC_CLONE`、回调看到的首字节与
源一致。

**过程中钉死的一件事**：编译看到的是**扁平化序列**，绑定时棋盘记录可能还读不到（实测源实例的配置
回读被推迟到第一次服务调用）。所以复制路径在标记副本时**同时把抄来的字节交给绑定**，而不是指望绑定
时能回读记录；源实例已绑定时优先用宿主手里的配置。这条对任何"绑定期就要看到某份配置"的语义都适用。

**M2 回补剩余**：`on_load`/`on_save`，最后是 move/rotate/resize 与编译类回调。
## M2 回补：`on_load` / `on_save`（2026-09-22）

**离线层**（`tests/native-component.cpp`）：记录里的配置被装上时触发一次并看到那些字节；只有默认值的
实例不触发；`on_save` 派发到所有活实例，且**回调里写配置会落到记录里**（这条同时是锁纪律的回归：
`on_save` 若在实例锁内运行，这个写入会死锁——实测第二版实现直接挂住，改成锁外运行才通过）。

**真机层**（`component-storage` 三个模式，PASS 行新增 `loads=/saves=`）：

```text
persist: launch 1 loads=0（夹具没有记录）  launch 2 loads=1  launch 3 loads=1（从文件读回）
migrate: launch 1 loads=1（迁移也算"来自存档"） launch 2 loads=1
reject : launch 1 loads=0  launch 2 loads=0（记录被拒绝 → 实例跑默认值，不算载入）
saves  : 每次启动都至少一次（探针自己的保存 + 游戏自己的保存）
```

三个模式分别 36s / 26s / 26s 通过。另记一个实现细节：编译看到的是扁平化序列，绑定期棋盘记录可能读不到
（回读被推迟到第一次服务调用），所以 `on_load` 在"真正装上配置"那一刻发，而不是死守绑定窗口——这也是
真机上 `loads` 一开始全为 0 的原因。

`save.schematic` 作为新别名进入符号画像（别名数 33 → 34），因为它就是 `on_save` 的第二个触发点。

# 元件图片 V5：元件栏卡片与抽屉预览（2026-09-26 晚）

玩家要"元件在元件栏里长成棋盘上的样子"。研究文档 `docs/research/component-icons.md` 已经把机制测清楚
（图片是游戏自己渲染的；往它请求的路径放文件没用；必须接住那次纹理请求），这一轮把它做成能力并验收。

**离线层**

* `tests/picture-path.ps1`（`src/picture_path.hpp`）：纹理路径 → 自定义元件 id 的判定规则。正例是
  真机实测那条（`…/asset/?snapshot_cc/com_custom_5058442071141929777.png`，正斜杠/反斜杠两种写法），
  反例覆盖内置分支 `?snapshot/com_custom_7.png`、缺数字、`.png` 之后多字节、大写后缀、标记词前后
  没有分隔符、id 超过 64 位、以及"长度之外还有字节不算路径"。
* `tests/float-icons.ps1`：随包的 22 张图逐张检查——192×192 透明画布、本体紫色整块宽 126–132 px
  （4.92 格 × 26 px/格 = 128）、最远像素离画布中心 ≥ 83 px（引脚在 ±3.0 格、半径 0.33 格）、
  白色文字像素足够多、画布中心对称。也就是说"图是元件外观、不是占位图"钉在数据上，而不是靠眼睛。
* `tests/component-render.cpp` 增加 V5 表与前缀检查（`set_picture` 的封装、`offsetof(set_picture) >=
  sizeof(V4)`），`tools/abi-snapshot.cpp` 与 `abi/windows-x64.json` 增加 V5 结构与
  `TC_COMPONENT_RENDER_API_VERSION_5` 的记录（1225 条全部一致）。

**真机层**（`tests/picture-playtest.ps1`，用例 id `component-picture`）

同一套三段流程各跑两次：注册**品红标记图**（22 张，按包内文件名生成，所以走的正是 Mod 自己那条
"按十进制 id 命名、逐个 set_picture"的路），再跑一次 `TC_FLOATOPS_PICTURES=0` 的对照。窗口
2536×1452，探针在每面各拍 2–4 帧进程内 GL 截图，用例逐帧数标记色：

```text
Run   Surface Frames Magenta Pins
green card         2   11610     0
green drawer       3   18141     0
green ghost        4       0   …（只数落点周围 ±160 px）
red   card         2       0     0
red   drawer       3       0     0
red   ghost        4       0   …
```

* **卡片**：注册时整排 22 项都换成标记图（品红），关掉注册后一处都没有——两面各自逐帧计数；
* **抽屉预览**：同上；
* **幽灵**：这个面用**四种输入**各测一次（随包真图 192×192、标记图 64/160/192 三种尺寸、以及完全
  不注册），落点周围都**只画引脚 + 名字标签 + 连线预览，没有本体**，逐像素一致。所以用例对这一面
  断言的是它真实的行为：拖到哪儿就在哪儿画出该元件的引脚（不是"图片到达了这一面"）。

**过程中钉死的两条**（研究文档 §5.4 有细节）：游戏会把重编码结果**写回它读的那个 PNG**，因此加载器
必须先把图复制到自己的目录再交给游戏，否则 Mod 的包内文件被外部改写、下一次 apply 会拒绝；给工厂的
Nim 字符串**必须活过调用**（放栈上会在游戏卸载纹理时访问越界，`fault.log` 里是
`outside any plugin callback: access violation`），现在它和副本一起长存在加载器的表里。

**旧调研用例的改动**（`tests/icon-playtest.ps1`）：加载器现在**拥有**纹理工厂（V5 就挂在那里），
插件再挂钩子会被拒绝（日志里是 `the loader owns this target for component render V5 picture control`），
所以这条研究用例改成：接受这条拒绝 + 加载器的 `picture control armed` 作为证据，并显式设
`TC_FLOATOPS_PICTURES=0` 保持它原本要测的"放文件没用"这件事不被 V5 干扰。

# 元件放置幽灵 V6：接管与它的结束信号（2026-09-26 夜）

V5 之后，玩家要的下一件事是"拖出来的幽灵也长成棋盘上的样子"。V6 用同一个绘制回调接管：
`TCComponentRenderApiV6::set_placement_preview`（SDK 侧按类型选择），加载器在游戏的
`redraw_clipboard_component` 路径读临时元件的 custom id、吸附格点与旋转，抑制游戏默认的"仅引脚/名字"
幽灵，并以 `instance_id == 0` 的预览帧调用该类型已注册的绘制回调。机制与两次实测见
[research/component-icons.md](research/component-icons.md) §5.2/§5.3。

**离线层**

* `tests/component-render.cpp`：V6 表与前缀检查（`tableV6`、`set_placement_preview` 的封装、
  `offsetof(set_placement_preview) >= sizeof(V5)`、`sizeof(V6) > sizeof(V5)`）与 `abi/windows-x64.json`
  里的 28 处 V6 记录（1225 条 ABI 行全一致）。
* 预览画的是**同一个绘制回调 + 类型默认配置**，所以它的外观由 `tests/float-icons.ps1`（随包的 22 张图
  就是同一套绘制代码的产物）与 M2 的真机用例覆盖；V6 自己不需要新的离线几何断言。

**真机层**（`tests/picture-playtest.ps1`，用例 id `component-picture`）

探针的 stage 流程现在跑**两次拖放**：第一次松手在元件栏上（取消），第二次**真的放到棋盘上**（成功），
每次各拍若干帧；两次都在加载器日志里留下 `placement preview taken over` → `placement preview ended`
成对的行。窗口 2536×1452，绿色一次（注册品红标记图）与红色一次（`TC_FLOATOPS_PICTURES=0`）对照：

```text
Run   Surface Frames Magenta Pins Body Stale
green card         2   11264    0    0     0
green drawer       3   18141    0    0     0
green ghost        4       0   44 1696     0     <- 拖动中：本体在
green after        1       0    3    0     0     <- 取消后：落点干净
green placed       1       0    7  458     0     <- 真的放到棋盘上
green settled      1       0    7  458     0     <- 停放指针后：落点仍是那一个元件
red   card         2       0    0    0     0
red   drawer       3       0    0    0     0
red   ghost        4       0   19  424     0
red   after        1       0    3    0     0
red   placed       1       0    7  458     0
red   settled      1       0    7  458     0
```

* **接管有效**：`ghost` 四帧都有 Float Ops 的紫色本体与红色引脚（`Body=1696`），且与 V5 图片注册
  无关——V5 的品红标记在幽灵区域始终为 0，两个接管面互不混淆；
* **结束信号**：`after` 一帧（取消后 1 秒）落点附近紫色为 0；`placed`/`settled` 两帧落点上有且只有
  棋盘实例本体，而被取消的那次拖放留下的格子 `Stale=0`；
* **生命周期计数**：脚本断言加载器日志里每一次 `taken over` 都有对应的 `ended`（一次运行里通常三次：
  抽屉里点开的那个元件也会起一次放置，加两次拖放）。
* **沙箱输入的已知偶发**：探针的合成鼠标输入偶尔不会被游戏接受（窗口没拿到前台），那一趟的拖放就
  没有开始——脚本把这种情形打印成 note 并只检查它真正看到的那几次接管，不把它读成幽灵缺失；
  配对断言（`ended >= taken over`）不受影响。

**回归证据（这条用例真的是照这个 bug 写的）**：把上一版加载器（只挂 `hide_clipboard`，SHA-256
`B12C17…`）放回 `dist\tc-loader.dll` 再跑同一脚本，两条运行都失败：

```text
green ghost 4  0 12 1696 / green after 1  0 3 424   <- 松手后幽灵留在落点
red   ghost 4  0 44 1696 / red   after 1  0 3 424
```

**被否掉的候选信号**（细节在 research §5.3）：`is_clipboard_visible`（拖动中恒为 0，游戏自己都走
"不可见"分支——用了它预览一帧都不画）与"`redraw_clipboard_component` 不再被调用"（一次拖动只调用
5 次，静止与结束不可区分）。

**部署**：`dist\tc-loader.dll` 与 `D:\p\game_engine.dll` 同哈希（`C5EB93…`），上一版现场备份为
`D:\p\game_engine.dll.bak-20260926-221735-before-clipboard-record-lifecycle`。
