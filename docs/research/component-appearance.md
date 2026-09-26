# 研究日志：自定义元件的"外观 / 占位 / 命中"是怎么来的

## 0. 原版元件的外观：一种类一张精灵图（2026-09-23 新增）

原版元件不是程序化画的，是**一张 PNG 精灵图**，而且**精灵名由 kind 直接决定**：EXE 里有一张
按 kind 顺序排列的 `com_*` 名字表（`0x00` 起），与 kind 枚举一一对应。新工具
`tools/component-sprites.js` 把这张表还原出来并与 `asset/component_sprites/` 对照：

```text
$ node tools/component-sprites.js          # 或 --csv
sprite table: 125 entries, 104 files in D:\p\asset\component_sprites
  0x00  com_none                       -
  0x01  com_off                        100x60
  0x02  com_on                         100x60
  0x03  com_not_bit                    140x60
  0x04  com_and_bit                    140x100
  0x05  com_and_3_bit                  140x100
  ...
  0x4f  com_cc_input                   180x180     ← Input Pin
  0x51  com_cc_output                  180x180     ← Output Pin
  0x5a  com_static_value               180x100     ← Static Value
  0x5b  com_screen                     1100x820
  0x5d  com_keyboard                   300x180
  0x76  com_ram                        620x380
  0x7c  com_deleted_15                 -
```

交叉验证：表里正好 **125 条**，与 `build/kinds.txt`（"built-in prototypes: 125"）逐条同名对得上，
说明索引就是 kind；被移除的 kind 用 `com_deleted_N` 占位，所以表不会错位。104 张图里没有
`com_custom.png`——**自定义实例（kind 0x4e）没有精灵图**，它是靠设计图缩略图 + 名字水印画的
（见下面 §2 的 32×32 4-bit 设计图）。

还有两类不在 kind 表里的图，说明外观是分层的：

- `com_imm_*`（`imm_counter`、`imm_register_bit/word`、`imm_delay_line_*`、`imm_ram`、
  `imm_static_value`、`imm_load_port`、`imm_store_port`、`imm_probe_*`）以及 `com_config_delay`、
  `com_ram_latency`、`com_level_input_arch/custom`、`com_level_output_arch`：
  状态/数值显示与配置界面的变体，按 kind 索引不到，属于另一条绑定路径（尚未定位）。
- `asset/component_addon/`：`buttons.png`、`segment_display.png`、`console_font.png`，是带子控件的
  元件（按钮、七段显示、控制台）用的附加图。

渲染侧的对应出口：`component.vert/frag`（主体）、`component_custom.vert/frag`（设计图缩略图）、
`component_button.vert`、`component_level_gate.vert`、`component_on_off.vert`（开关态）；实例级
`set_translation` / `set_rotation` / `set_color_override` / `set_color_shared` 与
`component95mesh95factory` 的 pin/highlight/comment 层。

**还没测的**：每 kind 的精灵在棋盘上的**锚点与缩放**（sprite 像素 → 板面格）、着色规则（主题色、
选中/悬停色）、引脚与标签的绘制位置、`com_imm_*` 的触发条件、以及附加图在实例上的排布。

### 0.1 锚点与缩放（2026-09-23 实测）

着色规则已经由 shader 定死，不用再测：`component.frag` 把精灵当**通道掩码**用——
R 通道 = 主体色、G = 描边色、B = 文字色，各自换成该实例 group 的调色板颜色
（BIT `#1E5F93`/`#1A3B8C`、WORD `#1E9363`/`#10612F`、IMMUTABLE `#C04E4E`/`#892F1E`、
UNLOCKED `#BDB02D`/`#675F11`，文字恒为白）；`component.vert` 用
`inst_group`/`inst_color_override`/`inst_color_shared` 再叠 defocused、overlapping、clipboard
三种半透明态。掩码被**拉伸到实例四边形**上，所以"精灵像素尺寸"不等于板上尺寸。

测量方法（不需要输入）：`TC_HITBOX_KINDS=0x03,0x0e,...` 让探针把内置 kind 放到空板上并截图
（进程内 `TC_MODLOADER_SHOT`），再用 `build/hitbox-dev/measure-sprites.ps1` 按调色板颜色框出
可见主体、换算成板面格，并与精灵画布/内容框对照。结果（8 种，板面格）：

| kind | 精灵 | 画布 | 内容框 | 可见主体（板面格） | 主体左上相对记录点 |
|---|---|---|---|---|---|
| 0x03 NOT(1bit) | com_not_bit | 140x60 | 51x40 @(57,10) | 2.45 x 1.92（量到 3.05x1.41，见下） | (-0.66,-1.02) |
| 0x0e Register | com_register_bit | 180x100 | 102x62 @(39,19) | 4.92 x 2.93 | (-2.46,-1.45) |
| 0x2e Constant | com_constant | 180x100 | 102x62 @(39,19) | 4.92 x 2.93 | (-2.46,-1.45) |
| 0x5a Static Value | com_static_value | 180x100 | 102x62 @(39,19) | 4.88 x 2.93 | (-2.46,-1.45) |
| 0x34 Push Button | com_push_button | 180x100 | 62x62 @(59,19) | 2.93 x 2.93 | (-1.45,-1.48) |
| 0x4f Input Pin | com_cc_input | 180x180 | 122x122 @(19,29) | 5.90 x 5.90 | (-3.44,-2.97) |
| 0x51 Output Pin | com_cc_output | 180x180 | 122x122 @(19,29) | 5.90 x 5.90 | (-2.46,-2.97) |
| 0x04 AND(1bit) | com_and_bit | 140x100 | 54x64 @(52,18) | 2.54 x 3.01 | (-0.82,-1.52) |

由此得到的锚点/缩放规则（对上面 7 种都成立，误差在测量粒度内）：

```text
板面坐标(sprite 像素 p) = 记录点 + (p - 画布/2) * 0.048
```

也就是：**精灵画布以元件的记录点为中心放置**，缩放约 **0.048 板面格/像素**（≈ 21 像素/格，
即这批美术是按 ~21 px 一格画的）；可见图形就是画布里的内容框，所以它的偏移完全由画师在画布里的
摆位决定（寄存器那类内容框正好居中，所以主体中心就在记录点上）。

未解/待复核：

- NOT(1bit) 那一行的自动读数与内容框不一致（多半是调色板匹配到了引脚标签的深色板），需要换判据复核；
- 引脚**点**与引脚**标签**不是这张图画的（另有 pin 层）：标签是深色底白字、画在主体外面
  （Register 的 "Enable/Value/Result"），位置与 kind 表里的引脚偏移一致；
- 四边形本身（画布拉伸到的那个矩形）只在推导里出现，还没有直接从 mesh 顶点读出来验证。

### 0.2 完整扫描：常数就是"20 像素 = 1 板面格"（2026-09-23 第二批证据）

把测量改成"主体色 ∪ 描边色"（只算主体色会漏掉 2–3 像素宽的描边，窄精灵因此系统性偏小）之后，
规律变成了干净的常数。**55 个种类的实测缩放中位数 = 0.0496 板面格/像素**，也就是：

```text
1 板面格 = 20 精灵像素      （scale = 0.05）
四边形 = 画布 / 20           （140x100 画布 → 7x5 格，180x100 → 9x5，300x180 → 15x9，180x180 → 9x9）
四边形以元件记录点为中心；可见图形 = 画布里的内容框
```

逐点核对（同一批 20 种，两轴都落在 0.049–0.050）：AND 0.0499/0.0494、delay line 0.0498/0.0498、
register 0.0498/0.0498、constant 0.0498、push button 0.0491/0.0498、level input 0.0499/0.0496、
equal/neg/add/ror 0.0499/0.0498、keyboard 0.0499/0.0495。少数偏低的是 47–55 像素宽的小精灵
（描边占比大，0.0478–0.0490），不是规则不同。

**明确的例外（精灵被非均匀拉伸，四边形不是画布/20）**：

| kind | 精灵 | 画布 | 实测可见主体 | 说明 |
|---|---|---|---|---|
| 0x76 RAM | com_ram | 620x380 | 30.08 x 11.64 | 横向 0.0500、纵向 0.0340 → 四边形约 31x13 格 |
| 0x36 Load port | com_load_port | 700x100 | 13.32 x 2.11 | 横向 0.0221、纵向 0.0406 → 长条端口被压扁 |
| 0x37 Delay line(word) | com_delay_line_word | 180x100 | 7.03 x 2.58 | 横向 0.0689 → 横向被拉长 |
| 0x2d 3 Bit Decoder | com_decoder_3 | 100x220 | 1.41 x 8.01 | 横向 0.0639 |
| 0x29/0x3b Level gate | com_level_* | 220x180 | 7.81 x 6.60/7.07 | 横向 0.0550，且属于 IMMUTABLE（红）组 |

另外两条还没解开：`com_not_bit`（0x03）的主体色匹配只框到薄薄一条（NOT 的三角形多半用的是另一个
通道），`0x0e Register` 纵向偏大（体内红色状态块或引脚标签干扰）。两者都要靠"直接读 mesh 顶点/
选中高亮矩形"来定四边形，而不是从内容框反推。

**证据文件**（可直接复算）：

```powershell
node tools/component-sprites.js                      # kind → 精灵名 → 画布/文件
# 每批 8–20 个 kind：放置 + 进程内截图 + 测量
$env:TC_HITBOX_SCAN='none'; $env:TC_HITBOX_KINDS='0x03,0x04,0x0e,...'
$env:TC_MODLOADER_SHOT='...\batchN.bmp'; $env:TC_MODLOADER_SHOT_DELAY='18000'
powershell -File build\hitbox-dev\run.ps1 -Tag batchN -Seconds 150 -Fresh
powershell -File build\hitbox-dev\measure-sprites.ps1 -Log ...\batchN.log -Shot ...\batchN.bmp -Csv ...\batchN.csv
```

合并结果在 `build/hitbox-dev-out/sprites-measured.csv`（本批 58 行、55 行含实测缩放；分组统计：
word 42、bit 11、immutable 2）——分组本身就是证据：它等于 `component.frag` 里 `inst_group`
选中的调色板，可以据此反推每个 kind 属于 BIT/WORD/IMMUTABLE/UNLOCKED 哪一族。

可直接用的换算（对一个 kind 的实例，已知记录点 board 处的坐标）：

```text
四边形 = 精灵画布 / 20   （板面格），以记录点为中心
可见图形 = 画布里的内容框 → 板上位置 = 记录点 + (内容框角 - 画布/2) / 20
颜色 = 按该实例的 group 取 component.frag 的调色板
```

### 0.3 引脚层（进行中）

引脚**点**不是精灵图画的，颜色也从截图里采到了（BIT 元件）：输入点 `#E5787D`(229,120,125)、
输出点 `#E53D5C`(229,61,92)；WORD 元件用的是绿色一族（与 `com_cc_input/output` 的画法一致）。
引脚**标签**是深底白字的小板，画在主体外面（Register 的 "Enable/Value/Result"）。两者的位置与
kind 表里给出的引脚偏移（`in0=(-1,-1)`、`out0=(2,0)` 等）一致，但"标签板的尺寸与避让规则"还没有
系统测量——下一步用同一套截图流程，按这两个引脚色 + 标签底色分别求包围盒即可。

测量脚本里那个 `drawnExtent`（整体绘制范围）暂时**不可信**：窗口 ±170 像素（±6.6 格）在
15 格间距的布局下会把邻居的图形算进来，表现为"等于整窗"。要用它就得把布局间距放到 20 格以上，
或者改成按引脚色/标签底色分别求框。

### 0.4 全量扫描结果（2026-09-23，106 个 kind）

把 125 条表里**有精灵文件**的 kind 全部扫了一遍（6 批 × 8 个 + 前面几批），合并表
`build/hitbox-dev-out/sprites-measured.csv`：

```text
placed & measured   : 106 kinds
with a scale reading: 101
scaleX median = 0.0496      scaleY median = 0.0495      （= 1/20 板面格每像素）
group histogram     : word 72, bit 26, immutable 3
```

**结论：1/20 这条常数在整个内置元件集合上成立**（中位数两个轴都落在 0.0495–0.0496）。离群项分两类：

1. **读数偏高（不是几何）**：`0x0e register`、`0x09 nor_bit`、`0x54 probe_wire_bit` 的纵向读成
   0.081 —— 这些元件体内还有一个**用同一主体色**画的状态块/数值显示，把包围盒往上/下撑开了。
2. **真正的非均匀拉伸（每 kind 自己的四边形）**：`0x36 load port`（横 0.022）、`0x37 delay line
   word`（横 0.069）、`0x2d decoder_3`（横 0.064）、`0x29/0x3b level gate`（横 0.055）、
   `0x76 ram`（纵 0.034）、`0x62/0x74 maker/concatenator 8`（纵 0.037）、`0x01 off`（横 0.060）。
3. **窄精灵残留偏差**：`0x3f–0x4b` 那一族 level input/output pin（宽 1–2 格、画出来只有 22–27
   像素宽），描边占比大，读数落在 0.042 附近——属于判据的粒度极限，不是规则不同。

所以"外形"层面现在能给出的稳定承诺是：

```text
多数内置元件：四边形 = 精灵画布 / 20，中心在记录点；可见图形 = 内容框
长条/多引脚元件：四边形是该 kind 自己的矩形，精灵按轴拉伸（表里已列出）
```

### 0.5 引脚标签的第一手证据 + 四边形直接测量（进行中）

把 AND 门放到板面中央并放大截图，可以看到引脚层的完整画法（`build/hitbox-dev-out/hl3-and.png`）：

```text
[Input 0] ●        ● [Output]
[Input 1] ●        ●
              AND
```

- 输入标签板在主体**左侧**、输出标签板在**右侧**，深底白字、圆角，右/左边缘贴着引脚点；
- 引脚点颜色：输入 `#E5787D`、输出 `#E53D5C`（WORD 元件用绿色一族）；
- 主体依旧是精灵掩码（红/绿/蓝通道上色），点/标签**不是**这张图画出来的。

**四边形直接测量还没成功**：探针加了 `TC_HITBOX_HIGHLIGHT=1`（放好元件后合成"按下 + 8 像素拖动
+ 释放"，希望游戏画出选中高亮，再用高亮矩形当四边形），但在本构建的沙箱里截图上**看不到可见的
选中高亮**——无法据此定四边形。下一步要么读 renderer mesh 的顶点（`component95mesh` 的实例顶点
缓冲），要么换一个必然重绘的触发（例如把元件拖进"重叠/剪贴板"状态，用 `color_mix` 的半透明态当
参照）。在那之前，四边形仍然是从内容框反推的（§0.2），并且例外项只到"每 kind 自己的矩形"这一步。

日期：2026-09-22。触发者：`examples/text-box`（板上文本框元件）。它需要三件今天做不到的事：
**整个框可拖动**、**看不到游戏自己画的东西**、**大小由自己声明**。本文记录为回答这三件事做的
测量，以及由此定下的接口要求（计划落在 [PLAN-custom-components.md](../PLAN-custom-components.md)
阶段 5/6）。工具：`tests/component-appearance-probe.cpp` +
`tests/component-appearance-playtest.ps1`（真机、沙箱、可选的原型手术开关）。

## 1. 原型里随"内部布局"变化的东西只有 7 个字节

构造三个定义并用 `importCircuit` 导入（都成功，**包括 0 节点 0 引脚的完全空定义**）：

```text
variant compact imported id=…7953 inputs=0 outputs=1 driverX=4   pinX=13
variant wide    imported id=…7954 inputs=0 outputs=1 driverX=4   pinX=120
variant empty   imported id=…7955 inputs=0 outputs=0
```

compact 与 wide 只差"输出脚在内部电路里的位置"（13 → 120）。整个 0x5a8 字节的原型里**只有 7 个
字节不同**：

```text
0x0010: 07 -> 04     名字长度（"compact"/"wide"），与几何无关
0x0108: 指针变化       → 指向 {1,0,0,0}：引脚表（1 个输出脚）
0x036f: 02 -> 00      64 位字段 0x368 的最高字节
0x050f: 00 -> 02      64 位字段 0x508 的最高字节
0x0570: 指针变化       → 指向 {64,0,0,0}：设计图缓冲区
```

空定义的两个指针都是 0（没有引脚表、没有设计图）。

## 2. 设计图不随内部布局变化

把 `+0x570` 指向的 64 个字按"每格 4 bit（value = cell & 0xF，color = cell >> 4）"解出来
（`component_custom.frag` 的读法）：

```text
compact design present=1 cells=7 bbox=(0,0)..(28,17)
wide    design present=1 cells=7 bbox=(0,0)..(28,17)   ← 与 compact 逐字节相同
empty   design present=0 cells=0
```

两边的格子列表完全一致（`(0,1)=4`、`(25,16)=2`、`(25,17)=1`、`(26,0)=1`、`(27,16)=1`、
`(27,17)=5`、`(28,0)=1`）。**结论：设计图是"把内部电路自动缩放塞进 32×32 格"的缩略图**
（`update_custom_design` 0x140175db0 里能看到取原型、复制解析出的电路、以及 0x7fff/0x807f
这一对坐标偏置），所以拓扑相同、间距不同的两个电路会得到同一张图。

截图侧的证据：三种形状放到同一块板上时，compact 与 wide 的都只是一小块（约 10px），空定义
**什么也没有**。也就是"空定义 = 游戏完全不画"是成立的，代价是那个元件同时失去可点区域。

## 3. 占位/命中不跟内部电路走（两条独立判据）

判据一：设计图逐字节相同（上一节），而占位就是这张图被缩放后的那一小块。

判据二：用**放置重叠**当探针（在离目标不同的偏移处放第二个元件，命令返回
`state=4 result=-6`，即 `TC_COMMAND_ERR_STATE`，就是"会被重叠"）：

```text
hitscan target=0 (compact, 内部 9 单位宽)  offset=(1,0) REFUSED   offset=(2,0) accepted …
hitscan target=1 (wide,    内部 117 单位宽) offset=(1,0) accepted  offset=(2,0) REFUSED …
```

拒绝只出现在离目标 **1–2 个板面单位**以内，两种形状的范围一样。

**结论：板上占位是一个跟类型绑定的固定小块，"把内部电路做大"不能让它变大。**要让"整个
文本框可拖"，必须能由类型**声明占位**。

（测量方法上的坑，留给下一轮：这条扫线里"放上去的梯子元件自己也会占位"，所以 d 成功会让 d+1
被它挡掉；干净的扫法要给每个偏移换一个全新的目标位置，或每次放置后删掉梯子——删除目前不在
命令总线里。）

## 4. 直接改原型是**不安全**的

加载器今天唯一能写原型的手段是"读原型 → `setPrototypeRaw` → `setCustomPrototype`"。把它用在
外观上会怎样？探针里做了三个变体（`TC_APPEARANCE_SURGERY=1` 开启）：

```text
appearance: prototype surgery on
（随后进程直接退出，连第一个变体的 "ready" 都没打印；fault.log 无记录）
```

- 把 `+0x368`/`+0x508` 置 0 或置 0x02<<56；
- 或者把 `+0x570` 指向的 512 字节整块涂成 `value=3,color=15`；

三种改动都让游戏在加载期退出。`+0x570` 的第一个字是 **64**，说明那块内存前面还有头/是序列式
结构，按"裸数组"写会连元数据一起覆盖。

**结论：外观写入必须由加载器走游戏自己的代码路径（`update_custom_design` 那条链），不能把裸
偏移暴露给插件。** 探针因此把手术开关默认关掉，默认路径只做"导入 + 上板 + 截图"的对照。

## 5. 对接口的要求（已写进计划阶段 5）

1. `tc.component.geometry`：类型声明棋盘占位与命中盒（两者可分开）——**这是"整框可拖"的唯一前提**；
2. `tc.component.render`：按类型关闭游戏默认绘制（设计图缩略图 + 名字水印）+ 每帧绘制回调，
   回调里给宿主算好的局部→屏幕变换与裁剪；
3. 两条写入都由加载器执行；插件只声明意图；
4. 不声明 geometry 的类型保持今天的行为（旧插件零改动）。

## 复现

```powershell
cd D:\p\tc-modloader
powershell -NoProfile -ExecutionPolicy Bypass -File tests/component-appearance-playtest.ps1
# 对照（安全）：四个变体上板 + 截图 build/appearance-out/board.png
$env:TC_APPEARANCE_HITSCAN='1'      # 放置重叠扫描（占位判据）
$env:TC_APPEARANCE_SURGERY='1'      # 原型手术，已知会让游戏退出，只用于复现 §4
```
