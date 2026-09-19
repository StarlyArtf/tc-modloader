# 插件界面（`tc_ui.h` / `tc_ui_draw.h` / `tc_ui_texture.h` / `tc_game_ui.h`）

逐项进度与后续接入位置见 [UI 推进记录](../research/ui-next-steps.md)。

两条独立的接入路径：

| 想做 | 用 | 状态 |
|---|---|---|
| 每帧浮动面板／工具条 | `tc::ui::panel()`（`on_frame`） | 已验证（`tests/ui-playtest.ps1`） |
| 主菜单里的整页界面 | `tc::ui::registerPage()` | 注册、绘制、生命周期、容器内输入与裁剪／滚动已验证；与板侧栏共用同一段内容区实现 |
| 电路板上的侧栏面板 | `tc::ui::registerBoardPanel()` | 注册、绘制、真实点击与场景生命周期已验证（`tests/ui-board-panel-playtest.ps1`） |
| 游戏外观的按钮 | `tc::game_ui::menu_button()` | 颜色／布局取自已锁定构建，**未做像素级截图比对** |
| 自定义图形／交互画布 | `tc::ui::Canvas`（`tc_ui_draw.h`） | 真实引擎连续 180 帧；图元、顶点坐标与颜色、移动／缩放、滚动、裁剪恢复回归 |
| 键盘按键 | `tc::ui::keys::pressed()`（`Key_*`） | 真实按键消息实测：Tab/Enter/Escape/字母到达插件控件，枚举值与 ImGuiKey 一致 |
| 文本输入／中文 | `tc::ui::inputText()` | 真实按键路径实测一个按键一个字符、不重不漏；UTF-8 中文码元（含 `WM_IME_CHAR`）可进入缓冲区；组合串与候选窗仍需人工验收 |

## 电路板侧栏插槽（`tc::ui::registerBoardPanel`）

第三条接入路径：面板画在**电路板自己的窗口里**、右边缘，随关卡出现和消失。宿主拥有
容器、输入归属和生命周期，插件只画内容——和主菜单页面同样的分工，但绑定的是游戏自己
构建的画面。

```cpp
// tc_mod_load 内，先 tc::ui::load(host)：
if (tc::ui::registerBoardPanel("main", "Board panel", draw) != 0) return 4;
// draw 的签名与页面相同：void(void* user, const TCFrame*, float w, float h)
```

**为什么点击不会穿透到电路板。** 宿主在游戏自己采样鼠标状态的那一刻之前，把面板画进
电路板窗口：`build_board_ui` 在 RVA `0x46b593` 调用 `igIsAnyItemActive`（返回后紧接
`0x46b5ae` 的 `igIsWindowBgActive`、`0x46b5e2` 的 `igIsWindowHovered`），这一对结果交给
`handle_io_on_board` 决定本帧是否处理板面鼠标。面板占用鼠标时，宿主把这个采样回答成
"有控件在活动"，于是落在面板上的按下／点击／拖动不会同时落到电路板。加载器是这些
入口的提供者，因此不需要插件参与。若某个插件自己也 Hook 了 `igIsAnyItemActive`（MinHook
会跟进跳转、把返回地址换掉），宿主改用一次短线栈回溯确认调用点，行为不变。

细节与边界：

- 面板只在电路板绘制时出现：宿主从电路板自己的每帧代码里调用插槽，关卡关闭就没有
  下一次调用，没有残留状态、也没有需要插件清理的东西。
- **窗口尺寸变化**：面板按当前窗口尺寸重排（关卡内改变窗口大小时同样成立，已有自动断言：
  改窗口后插件上报的绝对坐标随之变化、真实点击仍然命中）。加载器每次启动会记录一行
  `Display: dpi-awareness=… window-dpi=… monitors=… client=… surface=… ratio=…`，
  遇到缩放或多显示器问题时先看这一行。
- 每个插件最多 8 个插槽，同一插件内 `slot_id` 唯一；跨插件用
  `PushID(mod)/PushID(slot)` 隔离，同名插槽、同名控件互不影响。
- `preferred_width/height`（像素，0 = 默认）只是请求：宿主会缩到窗口内，
  `draw()` 收到的 `content_width/height` 才是可用内容区，请按它布局。
- **裁剪与滚动**：插件内容画在宿主的内容子区域里，超出部分被裁掉——被裁掉的项目在
  本构建里既不能悬停也不能点击（验证见 `tests/ui-board-panel-playtest.ps1` 的
  "clipped probe" 断言）。内容高于面板时，内容区右侧有一条宿主绘制的滚动条：
  拖动它即可滚动（`content_width` 已经扣掉这条滚动条的宽度，插件按收到的宽度布局即可）。
- 该子区域本身也是真正可滚动的窗口（本构建实测 `ScrollMax=1268`），插件若想自己控制
  滚动位置，可以在回调里调用引擎的 `igSetScrollY_Float` / `igSetScrollHereY`。
  注意：本构建的鼠标滚轮不会送到 ImGui（见下），所以滚轮滚动面板在这套后端上不可用；
  滚轮落在面板上时电路板也拿不到它（宿主仍把鼠标判给面板）。
- 面板只声明自己的矩形：落在面板外的点击仍然属于电路板（同一测试用"折叠线以下"的
  探针点击做了正对照）。
- **折叠**：面板标题行右侧有宿主绘制的 `-`/`+` 开关（整块可点，命中区 22×16 逻辑像素
  随缩放），点击把面板收成一条标题栏，再点展开。折叠状态由**宿主**保存，插件既不能
  自己折叠，也不能留下"折起来就找不回来"的状态。折叠期间宿主的裁剪区只有标题栏高，
  所以插件提交的内容既不显示也不能点击；但 `draw()` **仍会被调用**（每帧状态不必
  因此中断），内容区尺寸参数照常传入。插件若在 `draw` 里做重活，可以自己检查
  `tc::ui::` 的可用空间来决定是否跳过。
- 背景与标题行由宿主绘制（不透明，用本构建的页面底色），插件不要自己开窗口或画标题。
- 返回值：0 成功；−1 宿主不支持该能力或时序不对（旧加载器，插件应继续运行）；
  −2 定义非法（空指针、id 为空／超长／含 `###`、kind 未知）；−3 同一插件重复 id；
  −4 超过数量上限。

可运行示例：`dist/example.board-panel.mod`，源码在 `examples/board-panel/`：Ping 按钮、
复选框、波形画布和宿主归属说明。验证见
[验证体系](../verification.md#电路板侧栏插槽验证2026-09-18)。

## 键盘与输入法（2026-09-18 实测）

## 工具栏插槽（`tc::ui::registerBoardToolbar`）

把控件画进**游戏自己的工具栏**（电路板左侧那排工具按钮）里，做成"游戏里本来就有的工具"
那种样子：一个小按钮，鼠标悬停才展开弹窗。

```cpp
static void drawTool(void*, const TCFrame*, float availableWidth, float availableHeight) {
    tc::ui::ToolTile tile("##wirecolor");        // 工具本身：按游戏瓦片度量画的色块
    tile.colourIcon(current);
    /* 展开内容放普通窗口里（不要 Popup，见下）：位置贴着瓦片。 */
    if (auto panel = tc::ui::panel("调色盘###TCWirePalette", nullptr, {420.f, 560.f},
                                   {tile.origin().x + tile.size().x + 8.f, tile.origin().y - 8.f},
                                   1.f, 0, tc::ui::Cond_Always)) {
        tc::ui::text("每帧都会走到这里");
    }
}

tc::ui::registerBoardToolbar("palette", drawTool, nullptr, host);   // tc_mod_load 里注册一次
```

宿主的保证（多 Mod 场景就是靠这几条做到"通用且不打架"）：

| 事项 | 行为 |
|---|---|
| 位置 | 由加载器注入到游戏工具栏的**最后一个工具按钮之后**；工具列几何从游戏自己的按钮实测得到，插件不需要写坐标 |
| 顺序 | 所有插件的工具按 `Mod id → 插槽 id` 排序后依次绘制，与加载顺序无关，可复现；日志 `Board tools in the game's tool column (top to bottom): …` |
| ID 隔离 | 每个插槽在 `PushID(<mod id>)` + `PushID(<slot id>)` 作用域里绘制，两个 Mod 用同名控件不会串 |
| 上限与冲突 | 每插件最多 8 个插槽；同名 slot_id 返回 −3，非法参数 −2，宿主过旧 −1 |
| 生命周期 | 只在关卡里画（离开关卡即停），与侧栏面板同一套生命周期 |
| 绘制上下文 | 回调拿到的是**当前工具栏子窗口**的可用宽高（不是屏幕尺寸）；控件用 `tc_ui`/`tc_game_ui` 的控件即可获得游戏外观 |
| 字体 | 宿主在绘制插件工具前**借用游戏的正文字体**（注入点当前的字体是图标字体），所以 `tc::ui::text` 直接就能显示中英文；字号就是游戏的 UI 字号（2026-09-18 实测 45 px，与侧栏面板一致），要别的字号给 `panel()` 传 `fontScale` |
| 不要动样式变量栈 | 注入点位于游戏自己的 `PushStyleVar` 作用域内，`PushStyleVar/PopStyleVar` 会让游戏断言崩溃；字号请用 `igSetWindowFontScale`（即 `panel()` 的 `fontScale`） |

**注意**：工具本身应当很小（一个瓦片大小），展开内容用 `tc::ui::panel` 开一个普通窗口
（贴瓦片放、按需要设字号）。用 ImGui popup 承载展开内容也能画，但它继承工具栏那一列的
字体作用域、并且会和瓦片的 item hover 互抢，所以**验证过的做法是普通窗口**：真机截图见
[research/toolbar-tool-handoff.md](../research/toolbar-tool-handoff.md) §5，工具与单测在
`sdk/tc_ui_tool.h`、`examples/wire-palette/`、`tests/wire-palette-tool-playtest.ps1`。

键盘有两条互不相同的路，别混用：

| 想要 | 用 | 说明 |
|---|---|---|
| 物理热键（如 F8 开关面板） | `tc::ui::keyPressed(VK_F8)` | 直接读系统按键状态，ImGui 是否抢走键盘都能用；按键**仍然会送到游戏** |
| ImGui 键（跟随焦点） | `tc::ui::keys::pressed(tc::ui::Key_Tab)` | 走游戏自己的输入路径，与游戏看到的是同一份；输入框正在编辑时会被它吃掉 |

`Key_*` 枚举（Tab=512、Enter=525、Escape=526、A=546、F5=576 …）取自已锁定构建的
ImGuiKey，并**用真实按键消息核对过**，不是从符号表猜的。其余键值同样可用，直接用
ImGuiKey 数字即可。辅助函数：`keys::down/pressed/released/amount`、
`setKeyboardFocusHere(offset)`、`isItemFocused()`。

`tests/ui-keyboard-playtest.ps1` 用真实窗口消息（带扫描码的 `WM_KEYDOWN/UP`、
`WM_CHAR`、`WM_IME_CHAR`）驱动一个插件页面，实测结论：

- **按键可以到达插件控件**：`Keyboard probe: key Tab pressed/down/up`、
  `key Escape pressed`、字母键同理。
- **字符可以到达插件 InputText，且不重不漏**。测试对每一步都断言缓冲区的**完整内容**：
  `""` → `"abc"`（三次按键）→ `"abc\xe4\xbd\xa0\xe5\xa5\xbd"`（`你` 走 `WM_CHAR`、
  `好` 走 `WM_IME_CHAR`，都以 UTF-8 进入）→ `"abc你好z"`（单独发一条 `WM_CHAR`）→
  Escape 后回到 `""`。**输入法最终提交的那条路径（`WM_IME_CHAR`）是通的**。
- **Escape 先被输入框用掉**：`buffer ""`（ImGui 把文本回滚到获得焦点时的内容），页面
  没有被关闭。
- 一处曾经被误判的地方记在这里：第一版驱动同时手工发了 `WM_KEYDOWN` 和 `WM_CHAR`，
  于是每个字符出现两次，一度被当成"这套构建会重复投递字符"。实际原因是**按键消息本身
  就会被 Windows 转成 `WM_CHAR`**（消息循环的 `TranslateMessage`，和任何 Windows 程序
  一样），再多发一条 `WM_CHAR` 自然就多一个字符。三种发法的对照：

  | 发送内容 | 结果 |
  |---|---|
  | 只发 `WM_KEYDOWN/UP`（真实键盘的等价路径） | 1 个字符 |
  | 只发 `WM_CHAR` | 1 个字符 |
  | 两个都发（第一版测试的错） | 2 个字符 |

  所以真实打字是"一次按键一个字符"，`inputText` 不需要插件做去重。

### 中文输入法：需要人工确认的部分

组合串（preedit）、候选窗位置、选词这些必须用真实输入法，脚本无法可靠模拟。引擎导入了
`ImmGetContext`/`ImmSetCompositionWindow`/`ImmSetCandidateWindow`（IMM32.dll）并导出
`ImGuiPlatformImeData`，说明游戏后端有 IME 通路；因为插件页面／面板与游戏共用同一个
ImGui 上下文，理论上会顺带生效。

一键准备并启动**可见**的沙箱（不碰你自己的存档与 mods；日志在
`build\manual-sandbox\game\tc-modloader-data\loader.log`）：

```powershell
cd D:\p\tc-modloader
./tests/manual-ime-test.ps1                 # 准备沙箱 + 打印清单 + 启动可见窗口
./tests/manual-ime-test.ps1 -Hidden         # 只做冒烟检查，不开窗口
./tests/manual-ime-test.ps1 -Mods dev.ui-keyboard-manual,example.board-panel
```

等价的手工方式（脚本内部就是这些）：

```powershell
cd D:\p\tc-modloader
./tests/make-ui-sandbox.ps1 -Sandbox D:\p\tc-modloader\build\manual-sandbox -Mods dev.ui-keyboard-manual
$env:USERPROFILE = 'D:\p\tc-modloader\build\manual-sandbox\home'
$env:APPDATA = "$env:USERPROFILE\AppData\Roaming"
Start-Process 'D:\p\tc-modloader\build\manual-sandbox\game\Turing Complete.exe' `
  -WorkingDirectory 'D:\p\tc-modloader\build\manual-sandbox\game'
```

`dev.ui-keyboard-manual` 是探针的无驱动版本：主菜单有一个带输入框的页面，电路板右上角
还有一个同样带输入框、并且会记录自己看到哪些键的面板。

> **2026-09-18 人工验收通过**（用户实际操作）：页面与面板里的中文输入、选词上屏、Esc
> 取消组合都正常；候选窗**跟随光标**——日志里 `IME board panel composition=1970,259
> candidate=1970,259 style=0x32` 随每个字依次右移，删字时退回；板上打字没有触发元件
> 放置或仿真运行。唯一已知现象：**独占全屏时输入法候选窗出现会让画面闪一下**——
> 候选窗是 Windows 自己绘制的独立窗口，加载器无法从这一侧消除，可尝试无边框窗口化全屏、
> 调整输入法候选窗选项或关闭 exe 的"全屏优化"。

**A. 主菜单页面里的中文输入**（主菜单 → Keyboard probe）：

1. 启用任一含输入框的插件页面（如探针包），打开页面并点进文本框；
2. 切到微软拼音，输入 `nihao`：组合串应出现在文本框内，候选窗应贴在文本框附近
   （若候选窗跑到屏幕角落或窗口左上角，说明候选窗定位没有跟随插件控件）；
   探针会把系统要求的位置写进日志（`Keyboard probe: IME page composition=… candidate=…`），
   所以"候选窗在哪"不必只靠肉眼判断。
3. 按空格／数字选词：上屏后文本框内容是否正确、是否只出现一次（选区键不应被游戏当成快捷键）；
4. 输入法还开着时按 Esc：应取消组合，而不是关闭页面或触发游戏快捷键；
5. 在**电路板上**的面板里重复一次，并留意打字时游戏是否同时触发了板面快捷键
   （例如字母选元件、空格运行）——这属于"快捷键穿透"，目前尚未验证。

**B. 电路板侧栏面板里的输入与快捷键穿透**（进任意关卡 → 右上角 Board keys probe）：

1. 点进面板里的文本框，用拼音输入 `nihao` 并上屏；
2. 关键问题：打字时**电路板有没有同时反应**（字母被当成元件选择、空格让仿真运行／暂停、
   Esc 弹出菜单）。日志里的 `board panel saw key X` 只说明面板收到了键；板上是否也吃到，
   只能看画面；
3. 输入法还开着时按 Esc：是取消组合、关闭面板，还是被游戏菜单接走；
4. 观感：右缘滚动条拖动是否顺手、内容是否被裁在面板内、滚动条是否够显眼；
5. 面板是否挡住游戏自己的工具栏或组件菜单（非 2560×1600 的分辨率尤其值得看一眼）。

**C. 显示相关（尚未验证）**：多显示器或系统缩放不是 100% 时，页面与面板的位置、字号、
鼠标命中是否仍然对齐；桌面分辨率不是 2560×1600 时面板尺寸是否合适。

已经自动化、不必手工重复的部分：注册／绘制／生命周期、真实鼠标点击不穿透、裁剪与滚动
拖动、按键与字符到达、Escape 被输入框消费、UTF-8 中文码元进入缓冲区。

## 自定义绘图（`sdk/tc_ui_draw.h`）

绘图是可选扩展，加载时先调用 `tc::ui::load(host)`，再调用
`tc::ui::loadDrawing(host)`。后者独立解析 19 个导出；失败时
`drawingReady()` 为 false，`drawingMissing()` 返回缺失名单，不改变原有控件接口。
无需更新宿主 ABI，也不需要另建 ImGui 上下文。

```cpp
#include "tc_ui_draw.h"
// tc_mod_load 内：
// if (!tc::ui::load(host) || !tc::ui::loadDrawing(host)) return 2;

// 在页面回调内，或 on_frame 的可见 panel/Child 内：
{
    tc::ui::Canvas canvas("preview", {480, 280});
    if (canvas) {
        using tc::ui::rgba;
        canvas.rectFilled({0, 0}, canvas.size(), rgba(28, 31, 40));
        canvas.line({20, 30}, {200, 100}, rgba(255, 190, 70), 3);
        canvas.circleFilled({240, 140}, 40, rgba(70, 150, 230));
        canvas.bezier({20, 200}, {100, 40}, {300, 270}, {420, 100},
                      rgba(100, 220, 170), 3);
        canvas.text({20, 240}, rgba(255, 255, 255), "自定义图形 100%");
        if (canvas.dragging()) {
            auto localMouse = canvas.mousePosition();
            // 用画布内坐标更新自己的控制点、节点或选区。
        }
    }
} // 恢复裁剪后再画其他控件
tc::ui::button("保存");
```

| 能力 | 方法 |
|---|---|
| 线与矩形 | `line`、`rect`、`rectFilled`（支持圆角）、`gradient`（左上、右上、右下、左下颜色） |
| 圆与三角形 | `circle`、`circleFilled`、`triangle`、`triangleFilled` |
| 曲线与任意轮廓 | `bezier`（三次贝塞尔）、`polyline`（可闭合）、`convexFilled`、`concaveFilled` |
| 文字 | `Canvas::text`，UTF-8，使用当前游戏字体，不解释格式字符 |
| 裁剪 | 画布自动与当前窗口／子窗口裁剪相交；`Canvas::Clip` 提供可嵌套的局部裁剪 |
| 输入 | `hovered`、`active`、`clicked`（左键释放）、`dragging`、`mousePosition` |
| 坐标与尺寸 | `origin`（ImGui 屏幕坐标）、`size`、`toScreen`、`toLocal`；`contentAvailable()` 返回当前容器剩余空间 |

图形和鼠标坐标都以画布左上角为原点，单位是 ImGui 像素。内部使用
`igGetCursorScreenPos(Vec2*)` 取得原点，因此移动窗口或滚动子窗口时不需要手算偏移。
画布通过 `InvisibleButton` 占据布局与输入区域；ID 必须在当前作用域内唯一。
鼠标状态在创建画布时保存，不会被随后提交的控件覆盖。裁剪仅限制绘制，
自定义轮廓的精确命中检测由插件自己实现；画布的输入区域是矩形。

画布必须在可见的窗口／子窗口或宿主页面内创建，并在该容器结束前销毁，不能跨帧保存。
尺寸必须为正；非有限坐标、非正线宽、非法分段数等不提交。多边形最多 16384 个点，
显式分段最多 4096；圆的分段数 0 表示自动。填充轮廓要求屏幕坐标下顺时针、无自交、
无孔洞、不重复首点，`convexFilled` 还要求凸形；任意凹形用 `concaveFilled`。
图片与 GPU 资源管理由下面的可选 `tc_ui_texture.h` 提供。

可运行示例：`dist/example.drawing-demo.mod`，源码在 `examples/drawing-demo/`。
启用后从主菜单 **Custom drawing** 进入，或在电路板按 **F8** 打开浮动面板。
支持网格、线宽、拖动贝塞尔控制点、点击计数和重置。

验证：`tests/ui-draw.cpp` 检查可选加载失败、坐标转换、输入快照和异常退出时的裁剪配对；
`tests/ui-draw-playtest.ps1` 在独立游戏与存档目录内跑真实引擎，逐个断言图元产生顶点，
核对矩形顶点位置与颜色、嵌套裁剪及恢复、窗口移动／缩放、子窗口滚动，并持续 180 帧。
这些是几何与状态回归，不等同于截图验收或真实鼠标拖动验收。

## 图片与纹理（`sdk/tc_ui_texture.h`）

需要本次构建的加载器。`tc::ui::loadTextures(host)` 会检查宿主尾部能力，旧加载器
返回 false，仍可使用原有图形和控件。宿主用 Windows WIC 解码图片，上传到游戏当前
OpenGL 上下文；插件无需链接图像库、COM 或 OpenGL。

```cpp
#include "tc_ui_texture.h"
static tc::ui::Texture icon;
// tc_mod_load 内，先 load(host)、loadDrawing(host)：
// if (tc::ui::loadTextures(host)) icon.load("native/images/icon.png");

// 可见窗口／页面内：
{
    tc::ui::Canvas canvas("preview", {320, 200});
    icon.draw(canvas, {20, 20}, {180, 180});
    // 裁出左半幅，可加颜色／透明度；UV 反向即可翻转图片。
    icon.draw(canvas, {200, 20}, {280, 180}, {0, 0}, {0.5f, 1},
              tc::ui::rgba(255, 255, 255, 160));
}
// 也可作为占位和响应点击的图片控件：
if (icon.imageButton("icon", {64, 64})) { /* ... */ }
// 不再需要时 icon.reset()；离开对象作用域也会自动释放。
```

图片文件放在 `.mod` 包的 `native/` 子目录下；路径是包内 UTF-8 相对路径，支持中文。
不能通过此接口读取其他 Mod、绝对路径或 `../`。PNG 保留透明度；解码使用第一帧，
不自动播放动图。`createRgba(width, height, pixels, byteCount, filter)` 支持程序生成图片：
像素按从上到下排列，每像素 R、G、B、A 四个字节、非预乘 alpha、无行填充。
过滤可选 `TextureFilter::Linear` 或 `Nearest`。

`Texture` 不可复制，可移动构造；加载失败保留原图。`load`、`createRgba`、`reset`
返回 0 表示成功，−1 表示宿主能力／线程／阶段不支持，−2 参数或路径非法，−3 配额不足，
−4 解码或上传失败。所有操作及对象销毁应在主／渲染线程执行，不要每帧重新加载图片。

宿主按插件管理句柄，不允许跨 Mod 释放。释放立即撤销该句柄的所有权，但 GPU 纹理
延迟到后续 ImGui 帧才删除，因此已提交的本帧绘制仍有效；失败初始化的插件资源会回收。
每图最大 4096×4096，每 Mod 最多 64 张、128 MiB（含等待释放的纹理），编码文件上限
64 MiB。上传恢复游戏已有的纹理绑定和像素解包／PBO 状态。进程退出由操作系统回收。

示例 `example.drawing-demo.mod` 现在还提供 **Flip image / Reload images / Release images**，
展示 PNG 与 RGBA 程序生成图。测试见 `tests/ui-texture-playtest.ps1`：真实 GPU 像素回读，
透明度／方向／UV、中文 PNG 路径、损坏文件、线程限制、所有权、配额及延迟释放。

## 主菜单页面（loader 0.5.0+）

插件在 `tc_mod_load` 里注册页面，宿主负责容器（全屏窗口、标题、返回按钮）与
Mod／页面两级 ID 命名空间，插件只画内容：

```cpp
static void page(void* user, const TCFrame* frame, float width, float height) {
    tc::ui::text("page content");
    if (tc::game_ui::menu_button("Apply")) { /* ... */ }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE ||
        !out || out->size < sizeof(TCPlugin)) return 1;
    if (!tc::ui::load(h)) return 3;
    tc::game_ui::load(h);                    // optional: game-look widgets
    if (tc::ui::registerPage("settings", "Settings", page, nullptr, h) != 0) return 4;
    return 0;
}
```

| 约定 | 说明 |
|---|---|
| 坐标空间 | 页面回调在宿主的全屏窗口内、`PushID(mod)`/`PushID(page)` 已压栈；坐标相对**内容区**（面板标题行以下、滚动条以左） |
| 生命周期 | 页面只在本插件加载成功后才出现；失败／拒绝的插件不留入口 |
| 关闭 | 退出关卡或进入电路板会自动关闭页面；页面若抛异常，宿主保留返回路径并停用该页 |
| 已打开的页面 | 同一时刻只有一个；切换页面即关闭前一个 |
| 裁剪与滚动 | 内容画在与板侧栏插槽**同一段实现**的内容子区域里：超出被裁掉、被裁掉的项目不可点击，内容过高时用右缘滚动条拖动滚动 |
| 旧加载器 | `registerPage` 返回 −1，插件照常运行（`host.size` 检查不读越界字段） |

### 容器内输入：已验证

页面容器内的 hover 与点击都已实测确认：

```
DRIVER: container window size 2560x1600
DRIVER: full-region probe itemHovered=1 windowHovered=1
DRIVER: CONFIRMED - a click reached a widget inside the page container
```

同一轮里页面内容也搬进了带裁剪与滚动的内容子区域（与板侧栏插槽同一段代码）：内容
坐标现在相对内容区左上角，插件按 `content_width/height` 布局即可；驱动测试同时断言
`UI page content …`（内容区存在）与 `Host scroll strip …`（滚动条存在）。

这里曾经有一个**真实缺陷**：容器窗口只有 32×32（ImGui 的最小窗口尺寸）。原因是宿主把
`igSetNextWindowSize` / `igSetNextWindowPos` 声明成了指针参数，而本构建要求**按值传参**
（加载器自己的 Mods 弹窗就是按值调用、尺寸正常）。签名错了以后两个调用静默失效，窗口
退化成默认大小，页面内容全被画到窗口外——表现就是"页面看起来画出来了，但里面什么都
点不到"。改成按值后窗口恢复为 `2560x1600`，全屏 416 个采样点全部 hover 到容器。

诊断过程也踩过一个坑：早先用 `SetCursorPos` 把光标摆到精确坐标来点按钮，而这台机器上
有另一个程序在持续把光标拉回屏幕中心，驱动设的位置立刻被覆盖，于是扫描结果时好时坏，
一度被误读成"容器收不到输入"。现在的测试不再依赖光标位置：页面自己放一个覆盖内容区的
探测按钮，只要光标落在页面内任意位置它就会被 hover、被点击。

`tests/ui-page-playtest.ps1` 现在断言

- 两个插件各自注册了同名页面且都加载成功（ID 隔离的前提），
- 宿主把页面画了出来，且容器尺寸等于视口（`container window size 2560x1600`），
- 页面内的控件能收到 hover，点击能到达控件（全内容区探测按钮的实测结果），
- 插件内的 `on_frame` 与页面各自计数、互不抢帧，
- 没有回调异常。

剩下的只有视觉比对与"进关卡自动关页"两项待人工确认（见 `docs/verification.md`）。

## 游戏外观按钮（`sdk/tc_game_ui.h`）

`menu_button()` 用菜单自己的文字颜色 + 透明底框复刻主菜单条目，`framed_button()`
用按钮色系画有底色的次要动作按钮。颜色不是从截图猜的，而是从可执行文件里读出的常量：

```
COLOR_TEXT_NORMAL   0.7294 0.7294 0.7294   普通
COLOR_TEXT_HOVERED  0.9412 0.9412 0.9412   悬停
COLOR_TEXT_PRESSED  0.9333 0.7294 0.1961   按下
COLOR_BUTTON_{NORMAL,HOVERED,PRESSED}      0.9176/1.0/0.8549 偏红的框式按钮
```

主菜单函数 `build_main_buttons__...home95page_u48`（VA `0x140449df0`）确认了这一形状：
`igCalcTextSize` 定尺寸 → `igInvisibleButton` 命中 → `igTextColored` 上色，悬停／按下换色。

实现走的是 `igPushStyleColor_Vec4`（`ImGuiCol_Text` / `ImGuiCol_Button*`）+ 普通
`igButton`，主菜单自己也调用它（`0x140449f1b`）。旧探针曾卡死，不能据此认定插件
无法调用 draw list；现已用正确的类型化调用和真实引擎回归验证自定义绘图，见上文。

**没有**复刻：点击音效（需要主菜单的 presenter 对象，不给插件）、悬停的渐变过渡
（菜单自己有时间插值）。因此视觉上一致，但过渡是瞬时的。

## `tc_ui.h` 里刻意没有的东西

以下入口在本构建的导出表里存在，但**调用结果不可用**，因此没有包装进 SDK：
`igGetCursorPos`、`igGetWindowPos`、`igGetItemRectMin`、`igGetItemRectMax`
（按 out 指针调用返回 `0x80000000`）。

能用的替代：

| 想要 | 用这个 |
|---|---|
| 鼠标在**屏幕/游戏**坐标 | `tc::ui::mousePos()`（`igGetMousePos`，out 指针） |
| 控件自身尺寸 | `tc::ui::itemRectSize()`（`igGetItemRectSize`） |
| 控件在**窗口内**的起点 | `tc::ui::cursorPos()`（`igGetCursorPosX/Y`，两个独立 float） |
| 窗口是否被鼠标指向 | `tc::ui::isWindowHovered()` / `isWindowFocused()` |
| 行高 | `tc::ui::frameHeight()` |

`load()` 目前解析 **52 个**导出，全部命中；结构体里没有声明但未绑定的字段。
`igIsMouseDown_Nil` 曾经被误判为"只在加载器转发层"而摘掉，复核后确认
`tc_game_engine.dll` 也导出它，已恢复绑定（`examples/wire-palette` 用到
`tc::ui::isMouseDown()`）。

### 视觉验收（2026-09-18）

颜色常量与布局规则来自可执行文件的 `.rdata` 与反汇编，但**没有**做过与游戏真实按钮的
并排截图比对（沙箱里读不到该窗口的帧缓冲：`glReadPixels` 恒为黑，`PrintWindow` 只得到
背景色）。

人工验收结论：**当前观感可以接受，不要求与游戏原生 UI 完全一致**，因此不再作为待办。
已知差异：没有点击音效；悬停是瞬时换色，主菜单自己带过渡动画。若以后要更贴近，颜色
常量已列在文件头部，可直接对照调整。

原生插件用**游戏自己的 ImGui** 画界面：`on_frame` 在游戏主／渲染线程、每个 ImGui 帧
最多调用一次，插件在里面画面板。`sdk/tc_ui.h` 把这条链路上需要的东西一次性封装好：
解析引擎导出、类型化的控件函数、RAII 窗口作用域、面板定位与字号、热键。

它不引入新的 ABI：所有函数都由已有的 `host->engine_proc` 取得，头文件本身不改变
`TCHost`，因此现有插件与 0.4.0 的加载器不受影响。

## 最小用法

```cpp
#include "../../sdk/tc_ui.h"

static const TCHost* host;
static bool show = true;

static void frame(void*, const TCFrame*) {
    tc::ui::toggleHotkey(VK_F7, &show);
    const tc::ui::Vec2 position = tc::ui::topRight(320.f);
    if (auto panel = tc::ui::panel("我的面板###TCMyPanel", &show, {300, 0}, position,
                                   0.6f, tc::ui::kToolbarFlags, tc::ui::Cond_Always)) {
        tc::ui::text("每帧都会走到这里");
        bool on = true;
        if (tc::ui::checkbox("开关", &on)) { /* ... */ }
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < sizeof(TCHost) ||
        !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!tc::ui::load(h)) return 3;   // 缺哪个导出会写进加载器日志
    out->on_frame = frame;
    return 0;
}
```

## 为什么要有 `load()`

以前每个插件自己写一份导出名单，逐个 `engine_proc` 取地址，取到之后才发现少了谁。现在
`tc::ui::load(host)` 集中做这件事，并且**在加载期失败**：

| 行为 | 说明 |
|---|---|
| 全部命中 | 返回 true，控件函数可以直接调用 |
| 缺任何一个 | 返回 false，把缺失的名字写进加载器日志（`UI: missing engine exports: ...`），并可用 `tc::ui::missing()` 读回 |
| 还没 `load()` 就画 | 所有封装函数都是空操作，不会因为空指针崩溃 |

名单不是猜的：本构建引擎的导出表与机器码都核对过（见下方“已验证”）。若某次游戏更新
移除了其中某个导出，插件会在加载期被明确拒绝，而不是在绘图时崩在游戏里。

## 入口点清单

| 分类 | 封装 |
|---|---|
| 窗口 | `panel`（定位 + 尺寸 + 字号 + Begin 一步到位）、`Window` 作用域 |
| 布局 | `sameLine`、`newLine`、`spacing`、`separator`、`setCursorPos`、`setNextItemWidth`、`setNextWindowPos`、`setNextWindowSize`、`setNextWindowBgAlpha` |
| 文本 | `text`（UTF-8，不做格式解释）、`textDisabled`、`TextWrap` 作用域 |
| 控件 | `button`、`smallButton`、`checkbox`、`radioButton`、`sliderFloat`、`inputText`、`colorPicker3`、`colorButton`、`Disabled` 作用域 |
| 容器 | `Child` 作用域、`pushId`/`popId`、`openPopup`、`beginPopupModal`、`closeCurrentPopup` |
| 状态 | `isItemHovered`、`isItemDeactivatedAfterEdit`、`isAnyItemActive`、`isMouseDown`、`isMouseClicked`、`mousePos` |
| 视口 | `viewportSize`、`topRight`、`windowWidth`、`windowHeight` |
| 帧信息 | `frameCount`、`timeSeconds`（与 `TCFrame` 等价） |
| 热键 | `keyDown`、`keyPressed`（边沿触发）、`toggleHotkey` |

`Window` / `Child` / `Disabled` / `TextWrap` 是 RAII 作用域，构造时 Begin、离开作用域时
自动配对的 End。ImGui 要求**窗口的 End 必须无条件配对**（即使 Begin 返回 false 也要
调用），只有 `operator bool` 表示“这次真的可见、可以画内容”——这正是手写
`if (Begin(...)) { ... } End();` 容易写错的地方。

## 面板约定

| 约定 | 值 | 说明 |
|---|---|---|
| 工具条 | `tc::ui::kToolbarFlags` | 无标题栏、不可移动、自动尺寸、不写 ini；与 `cycle-guard`、`mod-inspector` 现用值一致 |
| 面板字号 | `tc::ui::kPanelFontScale` = 0.62 | 游戏默认字号对这类小面板偏大 |
| 位置 | `tc::ui::topRight(width)` | 贴视口右上角，距顶 12 px |
| 定位条件 | `Cond_Once` / `Cond_Always` | `Once`（默认）让用户能拖动；视口会变化时用 `Always` |

`panel()` 的 `size` 与 `position` 传 `0` 表示“交给 ImGui”（例如 `{300, 0}` = 宽 300、
高度自适应）；位置传负值表示不动它。

## 两个实测坑

1. **`igSetNextWindowPos` 是三个参数**（`pos`、`cond`、`pivot`）。引擎实现从第三个参数
   寄存器读 pivot，只传两个参数会把当时的寄存器残留当成 pivot。`tc::ui::panel()` 与
   `tc::ui::setNextWindowPos()` 总是显式传 pivot，插件不需要关心这件事。
2. **`igText*` 系列是可变参数**（printf 风格），字符串里的 `%` 会被当格式符解释。
   `tc::ui::text()` 走的是不做格式解释的 `igTextUnformatted`；`textDisabled()` 内部用
   `"%s"` 转发，所以两者都不会被字符串内容影响。

## 规则与边界

- 只能在 `on_frame` 或宿主页面绘制回调内调用：主／渲染线程。
- 不要创建第二个 ImGui 上下文；不要跨 C ABI 抛异常；不要阻塞渲染线程。
- 面板文字是 UTF-8，中文可直接写在字符串里（游戏字体已覆盖）。
- 热键用物理键状态，游戏窗口失焦时同样有效，按键也会照常传给游戏——请避开游戏已占用
  的键。
- 未验证：多显示器与 DPI 缩放、输入框（IME 中文输入）、表格与弹出窗口的实际渲染、
  暂停/存档界面下的绘制时机。`ui-playtest.ps1` 覆盖的是面板渲染、视口与鼠标信息这条
  主路径。

## 已验证

| 证据 | 来源 |
|---|---|
| 45 个导出全部解析成功、插件在真机加载 | `tests/native.ps1`（真实引擎进程内执行 `tc::ui::load`） |
| 面板在真实游戏的 ImGui 帧内绘制；视口与鼠标返回合理值 | `tests/ui-playtest.ps1`，日志行 `Mod Inspector: panel drawn; viewport=2560x1600 mouse=0,0 frame=1` |
| 窗口/控件/文本封装可用 | `examples/mod-inspector`（面板）、`examples/cycle-guard`（工具条 + 面板 + 复选框 + 禁用作用域 + 自动换行）、`examples/wire-palette`（取色器、颜色块、输入项状态） |
| 复选框、禁用作用域、自动换行等入口点本身 | 加载器自己的 Mods 页面用的就是同一批导出（`src/loader.cpp`） |
| 主菜单页面注册、绘制、生命周期、两插件同名页面 | `tests/ui-page-playtest.ps1 -DriverMode`（驱动用宿主自己记录的按钮矩形点击入口） |
| `ImVec2` 用隐式返回指针（sret）传回 | 实测：按“返回两个 float”调用时 `igGetMousePos` 先返回 `0,0`、后返回 `-2147483648`；改成 out 参数后返回真实坐标 `2392,164` |

`igGetMainViewport` 返回的 `ImGuiViewport` 布局按 `Pos` 在 +8、`Size` 在 +16 使用
（读 `float[4..5]`），实测 2560x1600 与真实分辨率一致。
