# 研究日志：主元件的鼠标命中到底走哪条路

## 结论（2026-09-23，真人手测确认）

**可拖区域 = 声明的 footprint 矩形，框内任意位置都能拖，包括游戏什么都没画的地方。**

在隔离棋盘上由用户手动确认过两种形状：`set_footprint(6,3)` 的**带引脚**类型，以及**完全
没有引脚**（0 入 0 出、文本框那种）的同类元件——两者都可以从框里远离图形、完全空白的角落按住
拖走；而它们被画出来的图形都只有一个小标签框加一个引脚点。探针把这些声明框实时画在了游戏画面上
（`TC_HITBOX_DRAW=1`，截图见 `build/hitbox-dev-out/boxes-ingame2.png`），用户就是对着青框测的。

对 M5 的影响：

1. **"整框可拖"不需要新的命中几何**。文本框只要声明与自身盒子一样大的 footprint，整张便签就能
   拖动——这条已经在 V1 里（`set_footprint`），不需要再发明 `set_hit_box`。
2. footprint 同时仍是**占位/碰撞框**，所以把框放大就是真的在棋盘上占那么大地方（这本就是便签想要的）。
3. 本文前面几节里"footprint 不扩大命中区域"的说法**作废**：那是被两件事误导——命令总线放置的元件
   当时根本没登记进棋盘的命中状态（见下），以及"选中集"读数会闩锁。
4. 自动化的"移动差分"网格（`TC_HITBOX_MEASURE=move` + 交替拖动）只能当**辅助**：它测出的区域
   比手测结果偏、还带斜切边，原因是交替拖动会与邻居的碰撞箱相互作用。**手感结论优先于它。**

下面保留完整的调查过程，作为"为什么之前会得出相反结论"的证据链。

日期：2026-09-22。上下文：M5 的 `tc.component.geometry` V1 只声明占位/放置矩形，`set_hit_box`
要等真正的命中路径被定位。本文记录两件事：**现有"命中"读数已被证明不可信**，以及引擎侧只读的
调用链地图（可以直接承接，不必重新猜签名）。

## 1. 旧读数不可信（新证据）

**先记装置本身的两个缺陷**（否则会把人为输入当成引擎行为）：

1. **没有隔离真实鼠标输入。** 探针用 `SetCursorPos` + `SetForegroundWindow` 驱动**真实光标**，
   所以测试期间任何人移动鼠标，采样点就会被顶掉。2026-09-22 的用户操作确认了这一点，它也解释
   了下方 run #2/#4 的抖动。现在探针在按下期间用 `ClipCursor` 把光标钉在采样点（1×1 裁剪矩形），
   并在每个采样点记录 `clean=0/1`：真实输入一旦出现，该点被丢弃、用例直接判失败，不再静默混入。
2. **按下之前没有先 hover。** 第一次地图运行（下方 run #1）里，同一个偏移先读 0、后读 1，离元件
   中心 40 板面格的空点也读成"已选中"。加上"按下前一帧先把鼠标移到目标点"之后，这种自相矛盾
   消失。所以 §1 描述的现象是**装置缺陷**，不能当成引擎"选中集有状态"的证据。

工具：`tests/component-hitbox-map-playtest.ps1`（探针模式 `TC_HITBOX_SCAN=map`）。

做法：注册两个只差 footprint 的类型（默认 / `set_footprint(6,3)`），在隔离沙箱里各放一个实例，
围绕实例中心按 `dx=-4..4`、`dy=-3..3` 共 63 点采样，外加 5 个对照点：
`(-4,-3)`、`(2,0)`、`(-4,-3)`、`(40,0)`、`(40,0)`。每个点都是"按下 → 读游戏选中集 → 释放 →
清空选中"，并在每个点前用 `clear_selections__modelZboardZboard_u8323` 与
`clear__modelZboardZboard_u8371` 清掉当前/上一帧选中集。

**第一次运行（按下前不做任何 hover 预移动）的对照点结果：**

```text
hit-default  offset=(-4,-3) ... selected=0
hit-default  offset=(-4,-3) ... selected=1   ← 同一个点，第二次读成了 1
hit-default  offset=(-4,-3) ... selected=1
hit-default  offset=(40,0)  ... selected=1   ← 离元件中心 40 板面格的空点也读成"已选中"
hit-default  offset=(40,0)  ... selected=1
```

同一轮里 `hit-default` 的网格是"前 25 点全 0，从 `(2,0)` 起全 1"；`hit-expanded` 从第一轮重复点
开始就全 1。按 §1 第 1 条，这一轮**没有隔离真实输入**，因此这个"第一个命中点之后全 1"的形状首先
要归因于当时的真实鼠标位置（游戏按上一次位置维持选中、清空只瞬时生效），而不是几何。

这一轮的形状（第一个命中点之后全 1、空点也读成已选中）**不能**用来推翻旧结论：它是装置缺陷的
产物，而不是几何。旧用例自己的日志里同样有这种残留（`hit-expanded` 的第一个点 `(1,0)` 报
`selected=0 key=1`，集合里躺着的是上一个类型），也属于同一类污染。

**加上 hover 预移动（按下前先把鼠标移到目标点、空悬一帧）之后，三次运行的对照点：**

```text
run #1  hit-default  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0     hit-expanded  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0
run #2  hit-default  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0     hit-expanded  -4,-3=0 2,0=0 -4,-3=0 2,0=1 -4,-3=0 40,0=0 40,0=0
run #3  hit-default  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0     hit-expanded  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0
run #4  hit-default  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0     hit-expanded  -4,-3=0 2,0=0 -4,-3=0 40,0=0 40,0=0
run #5  0 脏样本（ClipCursor + clean 标记生效）  两个类型的 63×2 网格全 0，对照点全部一致
```

run #2/#4 的异常网格（整片变 1、唯一命中落在 `(1,0)`）都发生在装置修好之前，按第 1 条归因于
未隔离的真实输入，不再算作引擎行为。

### 手势会决定"到底有没有选中"（本轮第二个发现）

修好装置后，两种**手势**给出不同的结果：

| 手势 | 结果 |
|---|---|
| hover（一帧）+ 只按下，不移动 | 干净运行（run #5、run #6，0 脏样本）：63×2 网格全 0，连元件中心 `(0,0)` 都是 0 |
| 按下 + 8 px 拖动（旧用例的手势） | 干净运行、`clean=1`、0 脏样本：`(1,0)` 不中、`(2,0)` 中，**两种 footprint 结果相同** |

也就是说，"按住并稍微拖动"才会触发选中，只按下不会——这是游戏手势的性质，不是几何。
用"按下+拖动"手势跑整张网格时，本轮被污染检测判废（136 个点里 18 个被真实输入顶掉），
所以**还没有干净的全网格数据**。

### 目前这批证据的强度

- 旧结论"真机已证明 footprint 不会扩大鼠标命中区域"**没有被推翻，但也没有被证实**：它在干净
  装置下可复现，可是只有 `(1,0)`/`(2,0)` 两个点，而且读数对手势敏感；
- 之前的"推翻"说法（含本文早期版本）不成立——那是装置缺陷造成的假象；
- 每次运行的地图追加进 `build/hitbox-map-out/map-history.txt`；只要出现任何 `?` 或脏样本，
  用例直接判失败，不再把污染平均进结果。

### 2026-09-22 第二轮：干净的全网格跑出来了，但读数仍不可复现

装置改成同时支持两种注入方式后（`TC_HITBOX_DEVICE=imgui | cursor`），拿到了第一次
**全 136 点、只有 4 个脏样本**的网格（抖动来自真人操作鼠标，已按规则丢弃）。手势与旧两点用例
完全一致（`TC_HITBOX_HOVER=0 TC_HITBOX_TICK=350`）：

```text
hit-default   dy=0  000000100     ← 唯一命中在 dx=2，即该类型声明的输出脚位 out0=(2,0)
hit-expanded  dy=0  000000000     ← 同一点不中
```

而**同样设置**下只跑 `TC_HITBOX_POINTS='1,0;2,0'` 的两点用例，两个类型都在 `(2,0)` 报
`selected=1`。同一装置、同一手势、同一坐标，跨运行给出相反答案，所以 §1 那句"旧结论在干净
装置下可复现"只对那一次运行成立：**目前的合成读数仍然不可复现，不能当成几何证据**。

### ImGui 注入（不使用真实光标）这条路的实测结论

目标是不再抢用户的鼠标：把位置写进游戏自己的 ImGui IO（`ImGuiIO_AddMousePosEvent`），按钮照旧
用 `ImGuiIO_AddMouseButtonEvent`。实测两点：

1. **窗口在前台时这条路不通。** Win32 后端每帧把操作系统光标位置再塞一次事件，且排在探针事件
   之后，游戏读到的是它。把窗口挪到桌面外（-4000,200）并保持前台后，日志里出现
   `mouse=(5658,163)` —— 正是用户真实指针 (1658, 363) 相对被挪走的窗口的坐标，而不是注入点。
2. **窗口不聚焦时注入生效，但棋盘不响应。** 不再抢焦点后，样本能读到注入的坐标本身
   （`clean=1`，`mouse=` 与目标点一致，真实指针也确认不在窗口内），但按住 + 拖动整张网格
   **一个 `selected=1` 都没有**。

结论：这条路保留为开关（`TC_HITBOX_DEVICE=imgui`），但默认回到真实光标；在弄清棋盘为什么不看
ImGui 位置之前，不能用它出证据。

### 手工读数暴露了更根本的问题：选中集是"粘"的

上一轮留下的手工用例（`tests/component-hitbox-manual.ps1`，不注入任何输入）实际记录了 40 次
真人点击。按"按下瞬间"的读数分析时，中间那个 12x6 类型在偏移 `(-4.53,-3.16)` 甚至
`(-15.78,-7.46)`（远在框外）都报 `selected=2`。这说明**按下瞬间的选中集只说明"上一次点击之后
还有什么被选中"，不说明这次点到了什么**；它同时解释了合成探针为什么会自相矛盾——探针在按下
瞬间读的正是这个滞后的集合。

手工探针因此改成每次点击记两行：`manual press`（按下前的选中集）和 `manual settled`（释放后
600 ms 内稳定下来的选中集），并打出每个类型的实时 `half=` 半宽半高、把选中集映射回类型名。
真正能当命中判据的是 `settled` 那一行。

### 2026-09-22 第三轮：合成装置被"正对照"证伪，不要再重建它

这一轮的目标是先证明装置能用，再谈几何。做法是给探针加一个**已知可选中/可拖动的正对照**：
用同一条命令总线放一个内置 AND 门（kind 0x04，`custom_prototype_id=0`），并用一个新的、不会闩锁的
观测量——**元件自身的坐标变化**（`TC_HITBOX_MEASURE=move`）：按住并拖动如果抓到元件，元件会跟着
走；抓到空白不会。结果：

1. **同一个坐标连按三次**（`TC_HITBOX_POINTS='2,0;2,0;2,0;1,0;2,0'`）：读数是 `0,0,1`，然后
   **本次运行之后每一个点都读 1**，包括离元件 40 格的空点和首次读 0 的同一个点。这叫闩锁，
   不是逐点命中；之前所有的"命中图"（含那个 `(2,0)`）都只是闩锁翻起来的时刻。
2. **内置 AND 门在同样的手势下纹丝不动**：中心 `(0,0)`、本体内部 `(1,0)…(2,2)`、
   以及它自己的输出脚位 `(2,0)`，`moved` 全是 `(0,0)`，选中集也多次保持为空。
   也就是说**装置连"已知能拖动的内置元件"都驱动不了**，那么它关于自定义元件的任何读数都没有
   意义。
3. 换过三种按键通道（ImGui `ImGuiIO_AddMouseButtonEvent`、`PostMessage(WM_LBUTTONDOWN/UP)`、
   `SendInput` 真实注入）、两种拖动方式（单帧跳 40 px、分 5 帧逐步移动）、三处摆位
   （左上角、同一行右侧、正中间偏上），结论不变。

摆位那一课来自用户：**选中一个元件会弹出它的面板，面板会盖住它下方的板面**，而左上角是游戏自己的
元件面板，两者都会吃掉点击——一开始把对照放在下面/左上角时"选不到"其实是这个原因。这条以后
设计任何板面点击实验都要先排除：点击目标必须在**既没有面板、也没有游戏 UI 的中间板面**。

结论：`TC_HITBOX_DEVICE=cursor` 这套合成装置**不能**用来测命中区域，别再往它上面加装置了。
可以复用的是它的两个副产品：`TC_HITBOX_MEASURE=move`（位置差分，不闩锁）和正对照摆位规则。
真正可信的两条路是：**真人点击的手工用例**（`tests/component-hitbox-manual.ps1`），以及
**静态跟读引擎自己的 hover 判据**——后者已经有落点：`handle_no_action_yet` 里
`get_component_id(state, point)`（@0x1402ec450）只是从 presenter 状态的 `+0x2c0` 那张
point→component 表里取值，真正的写入者才是"哪个元件在光标下"的定义。

### 板面截图：游戏给自定义元件画的东西（2026-09-22）

用户提醒"元件是点到它那个**图形**才能移动的"，所以别再推断图形在哪，直接用加载器自带的进程内截图
（`TC_MODLOADER_SHOT=<file.bmp>` + `TC_MODLOADER_SHOT_DELAY`，从 `igEnd` 调 `glReadPixels`，
完全不碰鼠标）把板面拍下来。探针新增 `TC_HITBOX_SCAN=none`：只放置、只报每个实例的
`board=(x,y)` / `origin_screen` / `pin_screen`，不注入任何输入。截图见
`build/hitbox-dev-out/board-drawn.png`（同目录 `board.bmp` 是原始帧）。看到的事实：

1. **`set_footprint(6,3)` 什么都没改。** 默认 footprint 的实例和 12x6 的实例画出来**完全一样**：
   一个小标签框（两字宽）+ 右边一个引脚点。12x6 的那个没有任何更大的外框或更大的图形。
   这就把 V1 的语义从"没测出差别"升级成了直接可见：**footprint 只影响占位/放置，不参与图形**。
2. **自定义实例没有被画出来的"本体"。** 图上只有标签框和引脚点，所以按"点图形才能拖"的规则，
   这种元件除了那一小块标签和引脚之外，本来就没有可点的区域——与手工探针测到的
   "只有 `(2,0)` 有反应"是同一件事。
3. 内置 AND 门的本体画得很清楚（约 4 板面格宽，中心正好落在记录坐标上），**而合成输入按住它
   本体正中拖动，仍然 `moved=(0,0)`、选中集不变**——所以合成装置的问题不在摆位，而在它根本没走通
   游戏拖拽本体那条输入路径。

推论（待真人点击确认）：`tc.component.geometry` V2 的 `set_hit_box` 如果只是"声明一个更大的
命中矩形"，很可能落空——真正要对齐的是**游戏画出来的那份图形**（renderer mesh / 原型图形），
而不是 footprint 或 shape 序列。

### 2026-09-22 第四轮：用户实测推翻了"合成输入无效"的结论

用户在自己的机器上（同一个沙箱、探针只挂只读钩子、不注入）得到的三条实测结果：

1. **探针用命令总线放的 AND 门一开始拖不动**；
2. **用户用游戏自己的元件菜单放了一个 AND 门之后，连探针放的那个也能拖了**；
3. **用户手放的 AND 门能拖，探针放的三个自定义小方块也能拖。**

也就是说：**元件是能点、能拖的，问题出在"命令总线放置之后的棋盘登记"**——放置命令只调用了
游戏的 `add_component`，而手动放置之后还跟着 `upgrade(presenter+0x1a3b8, 0x30)` 等步骤；
在棋盘的派生命中状态刷新之前，命令总线放的元件对鼠标是"不存在"的。这是加载器
`TC_COMMAND_BOARD_PLACE_COMPONENT` 的一个真 bug，也解释了本轮之前所有"点什么都没反应"的现象。

同时还要**更正上一节的一条结论**：探针用"元件坐标有没有变"来判断有没有拖动，而
`TC_HITBOX_MEASURE=move` 打出的 `posread=` 一直是 **0** —— 位置读取本身失败（元件记录在
棋盘序列被重建后不在原位，`resolveBoardObject` 报 STALE），所以 `moved=(0,0)` 是**读数失败**，
不是"没动"。同一轮里按 AND 门本体时选中集出现 `key=1`（`snapshot=1`），说明**合成输入是通的**。
上一节"合成装置连内置元件都驱动不了"的说法据此作废。

现在的图景是自洽的：

- 命中跟着**画出来的图形**走（用户判断 + 截图证据）：内置 AND 门有本体图形，点它能选中；
  自定义元件的图形只有一个小标签框和引脚点，所以只有那一小块能点；
- **footprint 不参与图形**：`set_footprint(6,3)` 的实例画出来和默认 1x1 的完全一样；
- 命令总线放置不会把元件登记进命中状态，**要先让棋盘刷新一次**（手动放一个元件即可）才会生效。

对 M5 的含义：`set_hit_box` 不能只是"声明更大的矩形"，要么让游戏**画出**覆盖整框的图形
（原型/设计侧），要么给这个元件加一个真正的可交互元素。而"整框可拖"的产品目标在此之前还要先
修掉放置后的登记问题，否则任何测量都会测到一个"鼠标看不见"的元件。

### 2026-09-22 修复：放置命令现在会重放游戏自己的登记步骤

按上一条追下去，缺的那一步就是游戏手动放置时紧跟 `add_component` 的那个
`upgrade(presenterSlot, 0x30)`（`presenterSlot` = 棋盘输入状态指针 + `0x1a3b8`）。加载器没有
别的地方能拿到这个指针，所以按它自己的惯例走**从游戏自身调用里学习**：`armBoardRegistration()`
钩住 `upgrade__presenterZcontext_u2766`（新别名 `board.after_place`）记住第一次看到的槽位，之后
`TC_COMMAND_BOARD_PLACE_COMPONENT` 在 `add_component` 成功后重放 `upgrade(slot, 0x30)`。
槽位还没学到时会打一行日志说明"可能要点一下棋盘才可点"，不静默失败。

验收（同一沙箱、同一位置、探针**不**做任何重放）：

```text
修复前  hit-and offset=(0,0)  selected=0  key=MAX     ← 按本体什么都不发生
        hit-and offset=(2,0)  selected=1  key=1       ← 只有引脚那格会选中
修复后  hit-and offset=(0,0)  selected=1  key=1 snapshot=1   ← 按本体就选中
        hit-and offset=(1,0)  selected=1  key=1 snapshot=1
        hit-and offset=(40,0) selected=0  key=MAX            ← 按空板取消选中
```

顺带解决了闩锁问题：登记正常的元件，按空板会**取消**选中，所以"选中集"重新变成逐点可用的读数
（之前的"永远 1"是未登记状态的产物）。位置读数（`posread`）仍然失败——元件记录在棋盘序列重建后
句柄失效——要用位置差分的话得改成每次重新抓一次对象快照再按实例 id 取值。

不过第一张"修好后"的网格仍然被闩锁带偏：扫描是逐行进行的，一旦某点命中，后面的格子全读 1
（`hit-and` 的 `dy≥-1` 整片 1、`hit-default` 的 `dy≥1` 整片 1）。所以探针加了
`TC_HITBOX_RESET=1`：**每个采样点之前先在远处空板上做一次"复位点击"**（不拖动），把选中清掉，
并把复位后的选中数记进日志的 `presel=`；只有 `presel=0` 时该点的读数才是"这次按键有没有命中"。

复位点击也没治好：游戏每帧重申自己内部那个选中，模型集合清空无效，`presel` 只能读到"点击还没被
处理"的那一刻。**真正可用的观测量是元件自身坐标**（`TC_HITBOX_MEASURE=move`），它不闩锁：
按住本体拖动 → 元件跟着走，按住别处 → 不动。为此修了三处仪表缺陷：

1. **对象快照要同时给元件和导线两个缓冲**。容量检查是"两者都够才通过"，只给元件缓冲时，一旦
   棋盘上出现导线（拖动就会留下），`capture_objects` 永远返回 `CAPACITY(-7)`；探针以前把
   `-7` 当成"读失败"，于是 `moved=(0,0)` 被误读成"没动"。现在两个缓冲都分配并重试。
2. 位置读取改为**每次重新抓快照**（按实例 id 找，找不到再按序列号回退）：拖动会换掉实例 id，
   旧句柄在读时已经失效。
3. 一次成功的拖动会把元件挪走（实测 40 px ≈ +2 板面格），网格会漂移并可能撞上邻居的碰撞箱
   （用户提醒过：碰撞箱会直接否掉拖动）。游戏的 `undo` 不覆盖移动，所以网格**每次交替拖动
   方向**，相邻两次的漂移互相抵消。

修好后第一次可信读数（内置 AND 门，`TC_HITBOX_MEASURE=move TC_HITBOX_ALTERNATE=1`）：

```text
offset=(0,0)   ← 本体正中      moved=(2,0)   ← 抓住了
offset=(0,3)   (0,-3) (2,0) (5,0) (-4,-3) (40,0)  moved=(0,0)，每次都 posread=1
```

也就是说：**"这次按键有没有抓住元件"终于可以直接测了**，而且内置元件的命中区确实跟着它的图形
（本体正中抓住，往外几格就不行）。这张表就是 M5 要的东西。

### 第一张可用的命中图（移动差分，2026-09-22/23）

同一套装置（`TC_HITBOX_MEASURE=move TC_HITBOX_ALTERNATE=1`，9×7 窗口，每次成功拖动后
交替方向以抵消漂移）跑出三张图。1 = 该点按住拖动**真的把元件拖走了**：

```text
内置 AND 门(图形 ~4x3)      默认 footprint 1x1          12x6 footprint
dy=-3  000000000            000000000                   111111111
dy=-2  000000000            000000000                   111111110
dy=-1  000010000            000000000                   111111100
dy= 0  000010000            000001000                   111111000
dy= 1  000010000            000000000                   111110000
dy= 2  000000000            000000000                   111100000
dy= 3  （1 个脏样本）       000000000                   000000000
```

三条结论：

1. **footprint 确实决定可拖区域。** 12×6 的类型在 ±4/±3 窗口里几乎处处能抓住，默认 1×1 的
   类型整个窗口只有一格能抓住。这与本文件前面"footprint 不扩大命中区"的说法**相反**，而那条
   说法本来就建立在本轮之前不可信的读数上。用户早先的观察（"小方块很难抓、大 footprint 的好抓"）
   与此一致。
2. 默认 1×1 那唯一的一格在局部 `(1,0)`，正好落在它的**标签图形**上；内置 AND 门是一竖条
   `x=0, y=-1..1`。所以小元件更像是"跟着画出来的那一小块"，而大 footprint 的整体都能抓。
3. 12×6 那张图的右下有一道斜切边（`x+y` 大约 ≤1 之外不中），需要一轮**逐条扫描线**的确认：
   交替拖动会与邻居的碰撞箱相互作用，边界形状暂时不能当成精确几何。

下一步就是这条：每个类型跑一条水平扫描线 + 一条垂直扫描线（不交替、单次拖动后复位），把边界
量出来；拿到边界之后再决定 `set_hit_box` V2 是"改 footprint 就够"还是必须动图形。

## 2. 引擎侧静态地图（只读，未运行游戏代码）

工具：`node tools/xref.js list|callers|refs|dis <symbol|0xVA>`（读 COFF 符号表 + `objdump`
反汇编）。下表是本轮实际跟出来的落点，地址对应 pinned 构建。

| 位置 | 符号 / 地址 | 读到的内容 |
|---|---|---|
| 交互区空间索引 | `tree__presenterZutilitiesZinput95utilities_u101` @0x14624d4e0；`insert__...u11479`；`delete__...u393`；`search__...u15509` | presenter 的 input utilities 模块里有一棵 R\* 树（`rsinsert_leaf` / `rstar_split_*` / `condense_tree` / `compute_bounding_boxes`），指针命中大概率先走它 |
| 谁往树里放 | `init_interactables__presenterZutilitiesZinput95utilities_u11792` @0x1402f6ea0 | 按元件 kind 分支生成交互区 |
| 交互区矩形来源 | `get_interactables_rect__...u16102` @0x1402f3600 | 内置 kind 的子控件偏移：`get_watcher_button_offset` / `get_push_button_offset` / `get_keyboard_record_button_offset` / `get_address_jump_button_offset` / `get_buffer_edit_button_offset` / `get_buffer_scroll_bar_offset` / `get_endian_button_offset` / `get_dram_button_offset` |
| 自定义元件的分支 | `get_interactables_rect` @0x1402f3767 `cmp dl,0x4e`；`init_interactables` @0x1402f74f7 `cmp BYTE PTR [rax+rdx*1+0x8],0x5a` | kind `0x4e`（自定义元件）只把它**内部元件的 kind 0x5a** 元素（经 `get_custom_position__modelZboardZcustom95prototype_u78` + `rotate__presenterZrendererZvector2_u85` 变换到屏幕）放进树 → 即设计图里的按钮/引脚类元素，**不是主元件本体** |
| 原型矩形的点包含 | `shape_contains__modelZboardZcustom95prototype95list_u681` @0x140126b40 | 取原型的局部矩形序列（复制体 `+0x48` 长度 / `+0x50` 数据，与 `src/component_geometry.hpp` 的 `kShapeSequenceOffset`/`kShapeStorageOffset` 一致），按元件旋转 `rotate__modelZmodel95types_u1542` + 位置 `plus__` 变换后做矩形包含；**唯一调用者是 `can_add_wire_from_pos__modelZboardZboard_u28427`**（走线时避开元件），不在选择/悬停链上 |
| 占位与放置 | `create_obstruction_cache__modelZboardZboard_u1602` @0x140146340 → `is_overlapping__modelZboardZboard_u15790`；`board_can_place_component__modelZboardZboard_u14854` | 与"放置重叠"判据一致；`handle_no_action_yet` 每次也会调一次 `create_obstruction_cache` |
| 点击/选择链 | `handle_no_action_yet__presenterZuser95inputZboard95ioZactionZnone_u1427` @0x14034ecc0：+0x17fb 调 `component_panel__presenterZutilities_u15969`，+0x18e3 调 `select_component__presenterZutilitiesZhelper95functions_u4505`；`get_component__...u2188` 只是"序列索引 → 元件记录"取值器 | 选择走 kind 分支构造的 panel 结果 + `select_component(state, board, id)`，不是对 shape 序列做点测试 |
| 组件级几何/高亮 | `focus_components__presenterZupdate95state95common_u5084` @0x140473700 遍历 `component_mesh_factory`；`redraw_custom_component__...u1101` @0x14046e7b0 用 `initTransform2D__presenterZupdate95state95common_u40` + `set_design_values__...custom95mesh_u1380` | 组件级几何来自 renderer 的 mesh，而不是原型矩形序列 |
| presenter 快照 | `get_snapshot_rect__presenterZcomponent95snapshotZsnapshot_u10` @0x14038d5a0 | 它就是**原型 shape 序列**的包围盒；调用者是 `update_state_snapshot` / `build_editor` / `get_label_offsets`，属于缩略图与标签布局 |

可复用的结论：`+0x48/+0x50` 这条局部矩形序列在引擎里有三处消费者——**放置/占位**
（`create_obstruction_cache`、`board_can_place_component`）、**走线避让**（`shape_contains`）、
**presenter 快照/编辑器缩略**（`get_snapshot_rect`）。目前**没有**证据表明选择/悬停会读它；
但按 §1，也还没有可用证据说它一定不读。

## 3. 下一步：先做出可信的命中读数，再定位路径

### 渲染网格这条路的第一手结果（2026-09-22）

`hint_at_component__presenterZutilities_u42972` **不是命中查询**：它把 `rdx` 的 variant 塞进
presenter 的 hint 槽（`eqsink` + `is_same_hint_object__presenterZcontext_u1915`），再用
`getTime`/`toUnixFloat` 做节流，是"提示气泡"而不是"这个坐标上是哪个元件"。

renderer 的 `multi95mesh` 里每个网格类型都有一组 `compute_offset__..._uN`，但它们**只是常量**
访问器（例如 `compute_offset__...component95custom95mesh_u21` 把 `TM__kV3DnggTVNTmpeQtuoHc4g_72+0x58`
的一个 8 字节常量写进 out；`..._u153` 写 32 字节 + 一个整数 6）。它们描述网格类型的固定偏移，
**不含每个实例的几何**。所以这条路的下一步要落在实例侧：`component95custom95mesh` 的实例记录
（`set_transform2d__...u1127`、`set_color_override__...u1222`、`set_design_values__...u1380` 写入的
那一段）以及它的顶点缓冲，才能读出"游戏眼中这个元件有多大"；`selection95mesh` 的实例同理，
那是选中/悬停高亮，最可能就是命中几何的真身。

实例侧的布局线索（2026-09-22 反汇编）：`set_design_values__...u1380` 的 `rcx` 是 mesh、
`r8d` 是实例序号，函数体先 `shl ebx,0xa`（序号 × **0x400 = 1024 字节**）再取 `[rcx+0x1c]`，
说明**每个实例记录 1 KB、以 dword 游标在 mesh+0x1c 指向的缓冲里**；
`set_transform2d__...u1127` 的签名同样是 `(mesh, transform, instanceIndex)`，并且会读
`scalar_attributes__...component95custom95mesh_u145` 这个全局描述符。下一轮从这里读实例记录。

1. **灵敏度**：让探针在"已知可选中的目标"上读到 1。已经有一个：旧用例手势下 `(2,0)` 能读到
   选中。**2026-09-22 更新：这一条不再是主要缺口。** 同样的手势已经跑完整张网格（见 §1 第二轮），
   结果是跨运行互相矛盾，而不是读不到 1；合成手势本身就不是可信的判据。现在的首选换成手工用例：
   真人点击 + `manual settled` 读数（每点之间先点一下空白处清选中），再用合成探针去复核手工结论。
   合成网格仍可跑，但它需要约 2.5 分钟独占鼠标，**动手前要先问用户**。
   **2026-09-22 第三轮再更新：合成装置已被正对照证伪（见 §1 第三轮），不要再跑合成网格。**
   顺序改成：手工用例拿结论 → 静态跟读 engine 自己的 point→component 表支持它 → 设计 V2。
2. **稳定性**：同一点重复读三次必须一致；离元件 ≥8 格的空点必须读到 0（`(40,0)` 对照点就是
   为此加的，现在稳定通过）。跨 run 也要一致：`component-hitbox-map-playtest.ps1` 每次运行都
   追加 `build/hitbox-map-out/map-history.txt`，本轮已经抓到过一次跨 run 不一致（§1 run #2）。
3. 有了 1+2 再跑 63 点网格；这时"默认 footprint 与 12×6 footprint 的格子是否相同"才有意义。
4. 若两种 footprint 仍然一致，按 §2 的顺序往下跟：`handle_no_action_yet` 里 `component_panel` /
   `select_component` 的实参，再看 renderer `component_mesh_factory` 的顶点范围（高亮几何就在
   那里），必要时用只读探针读 mesh 包围盒。
5. 只有命中路径确定、并在 0/90/180/270 四向都验证过，才设计 `tc.component.geometry` V2 的
   `set_hit_box`；写入仍必须走游戏自己的复制/更新/析构链。

注意区分"没有命中区域"与"我们的输入没触发选择"：修好读数后连元件中心 `(0,0)` 都读到 0，
与"整框拖不动"的现象一致，但第 1 条没做完之前不能下这个结论。

## 4. 复现

```powershell
cd D:\p\tc-modloader
# 网格 + 对照点（本轮新证据；探针内部会设 TC_HITBOX_SCAN=map）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-hitbox-map-playtest.ps1
# 旧的两点用例：保留为回归，但不要再当成命中证据
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-hitbox-playtest.ps1
# 只读静态查询
node tools\xref.js list custom95prototype
node tools\xref.js callers shape_contains__modelZboardZcustom95prototype95list_u681
node tools\xref.js dis init_interactables__presenterZutilitiesZinput95utilities_u11792
node tools\xref.js dis handle_no_action_yet__presenterZuser95inputZboard95ioZactionZnone_u1427
```

日志里可用的关键字：`hitbox: scan=map points=68`、`hitbox: maprow <type> dy=<n> <01 串>`、
`hitbox: cleared <type> after=(dx,dy) selected=<n>`。
