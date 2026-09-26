# 研究日志：板上文本框元件（0 脚形状、板上绘制、斜体）

日期：2026-09-22。目标：做一个"在电路板上写说明"的元件——玩家从游戏自己的元件列表放置它，
点元件上的编辑按钮输入文字，能设背景色（含透明度）、字号、加粗、斜体、文字颜色，并且这些设置
跟着板子留下来。本文记录这一轮量到的事实，以及由此定下的实现形状。

成品：`examples/text-box/`（包 `dist/local.text-box.mod`），真机用例
`tests/text-box-playtest.ps1`。

2026-09-23 更新：选中便签后会出现四个角点和四个边点，可直接拖动调整每实例的可见最小宽高。
geometry V2 让文本框每帧把最终可见框作为逐实例交互 footprint 上报；宿主接入游戏原生点查询，
所以可见框内任意位置都能原生选择/拖动/删除。类型级 8×4 footprint 仍只负责占位和放置碰撞。

## 1. 真 0 进 0 出：游戏收，但绑不上实例

> **2026-09-23 更新（结论已变）**：本节的"绑不上实例"在本构建上不再成立。同一形状
> （`inputs=0 outputs=0`）现在真机日志里会照常出现
> `Native logic: registered custom 0x544558545f303031 inputs=0 outputs=0 shape=0in/0out v2 state=0`
> 与 `Native logic: bound instance …`（一次运行 7 个实例全部绑定），元件也照常出现在
> `tc.board` 的元件序列里（`matched=4 notes=4`）。差别在加载器的脚手架：现在
> `component_definition::encode()` 为 0 输出补的那一个悬空驱动门是**真节点**，编译器会把它编进平坦
> 序列，桥接因此有东西可绑（见 `src/component_definition.hpp` 的 `validShape` 注释与
> `if (m == 0) w.component(0x12, 4, 0, 0x4000, "", 1);`）。`examples/text-box` 已因此改回**真 0 脚**
> （需要旧形状时用 `TC_TEXTBOX_OUTPUT_PIN=1`），验收见
> [../verification.md](../verification.md) 的 "文本框改回真 0 脚" 一节。下面这段保留为当时的记录。

先把加载器那道"两个方向不能同时为空"的门去掉（`src/component_definition.hpp` 的
`validShape`、`src/native_component.hpp` 的 V2 校验、`src/native_logic.hpp` 的
`supportedScaffold`），再用 `tc.component.types` 注册一个 0 进 0 出的定义。真机日志：

```text
Native logic: registered custom 0x544558545f303031 inputs=0 outputs=0 shape=0in/0out v2 state=0
```

**导入成功**（`register_definition` 返回 0，原型可读回），板上也确实出现了这个元件，位置、
移动、旋转、删除都正常。但同一个探针在放好元件、跑了几十帧之后**没有**任何
`Native logic: bound instance …` 行，`tc.component.instances.enumerate` 也一直是空。
原因不是加载器：**没有任何引脚的元件在板的网络里不可达，游戏的编译器不会把它编进平坦序列**，
桥接因此永远看不到它。

这条结论决定了配置怎么存：

| 方案 | 结果 |
|---|---|
| 元件配置（`tc.component.storage` + `config_schema`） | **不可用**：它按绑定的实例句柄写元件记录，装饰元件没有实例 |
| 元件记录尾部表（`save.custom_tail_set`） | 同上，写入路径属于绑定的实例 |
| 本 Mod 自己的存档（`plugin-data/notes.txt`） | 可用；键 =「板面标识 + 元件 64 位 id」 |

最终形状因此是 **0 输入 + 1 根悬空输出脚**：声明 0 门 0 延迟，回调只写 0，脚从不接任何东西，
且它相对元件中心在 (+2,0)，正好落在文本框自己的底板下面——玩家看到的还是一个没有脚的方框。
（引脚几何来自生成脚手架的布局，见 `src/component_definition.hpp` 的 `w.component(0x51, 13, …)`。）

## 2. 板上绘制：坐标与字号

| 事项 | 结论 | 依据 |
|---|---|---|
| 板面 → 屏幕 | 游戏自己的 `world_pos_to_screen_pos` 返回**归一化**坐标；乘 ImGuiIO 的 DisplaySize（`io+8`）得到像素。取 (0,0)/(1,0)/(0,1) 三点即得仿射矩阵 | 板面网格 Mod 已实测；本次日志 `text-box: board unit 25.6 px` |
| 画在哪一层 | 主视口 background draw list：盖在板面之上、所有面板之下 | 板面网格 Mod 的同一层 |
| 任意字号 | 本构建的 cimgui **没有** `ImFont_RenderText` 的可用 C 入口（导出的包装器签名与 cimgui 头不一致，直接按头调用拿不到参数）；`igPushFont(font)` 又只认字体自带的 FontSize。可行做法：`ImDrawList` 共享数据里就放着 AddText 要用的字体与字号——反汇编
`ImDrawList_AddText_Vec2` 得到 `draw_list+0x38` 是共享数据、`+0x18` 是 Font、`+0x20` 是 FontSize；临时换掉、调用 `ImDrawList_AddText_Vec2`、立刻换回 | 本次真机截图：中文与不同字号都正常 |
| 字体 | 游戏字体表（别名 `ui.fonts`）里 `[1]` 是 NoroshiCode_Bold、`[2]` 是 NoroshiCode_Regular；两者都带中日韩字形（截图里的中文正常） | 日志 `Tool column text font: defined_fonts[2]=…` + 本次截图 |
| 加粗 | 用 `[1]` 那张面 | 截图 |
| 斜体 | 游戏**没有斜体字面**，用"逐条 1 像素横带 + 每条按高度做水平位移"合成：每条把整行重画一次并按 `slant*(h/2-y)` 平移，位移步长 <1 px，肉眼是斜体而不是阶梯 | 截图（斜体行） |
| 文字量宽 | `ImFont_CalcTextSizeA`（本构建的 ImVec2 返回走隐藏指针，与 `igCalcTextSize` 同约定）；量不到时退回"按最小宽度排版" | `measured "MMMM"@20 = 36.9 px` |

## 3. 游戏自己画的名字水印

自定义元件在板上由 `component_custom.vert/frag` 绘制：一张 32×32 的"设计图"，外加**元件名字**
的水印。实测：

- 用 `TC_TEXTBOX_NAME=' '` 注册时水印消失 → 水印就是原型的名字；
- 给 `shape_svg`（原型 `+0xb0`）填一段 SVG **不会**去掉它，只影响元件栏图标；
- 水印画在文本框**下面**，所以默认背景取近乎不透明（alpha 238）并给盒子一个最小尺寸
  （宽 6.5 板面单位、高 1.5 板面单位），把水印盖住。玩家把透明度调低时它会重新透出来——
  这是游戏自己的绘制，插件无法关掉。

## 4. 还没有做的

- 配置写进**原理图文件**：0 脚形状绑不上实例（§1），但**1 根悬空输出脚会绑上**——最后
  一次真机日志里 `Native logic: bound instance 0x3e3c6b5bffc1463 of custom 0x544558545f303031`
  与 `emitted 1 callback(s)` 都在。也就是说对当前这个形状，`tc.component.storage` 是可用的：
  把本 Mod 现在用的 `plugin-data/notes.txt` 换成元件自己的配置记录，就能顺带拿到"复制元件
  时配置一起复制"（`on_clone`）与"文字跟着原理图文件走"。这一轮没做，是因为配置通道要重新
  接一遍并再做一次真机回归；
- 复制文本框时把文字一起复制（副本是新的元件 id，本 Mod 的键因此找不到记录）；
- 文本框自身的旋转（游戏可以旋转元件，盒子目前始终水平）。

## 复现

```powershell
cd D:\p\tc-modloader
powershell -NoProfile -ExecutionPolicy Bypass -File tests/text-box-playtest.ps1
# 产物：build/text-box-out/board.png，日志断言见脚本尾部
```
