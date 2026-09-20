# 工具栏插槽 / 导线调色盘：交接说明（2026-09-18，文字问题已解决）

## 0. 一句话现状

接口、位置、绘制、悬停展开、鼠标移开收起都已可用，**"展开界面没有文字"已经解决并验证**：
根因是插件控件被注入到工具栏时，当前的 ImGui 字体是游戏的**图标字体**
（`Icon_Complete.ttf`，没有拉丁/中日韩字形，所以只画字形不画矩形→矩形在、文字全无）。
宿主现在在绘制插件工具前借用游戏自己的**正文字体**，插件侧不需要做任何字体处理。
证据：`build/wire-palette-tool-out/open.png`（真实鼠标压在瓦片上，展开面板里中英文都正常）。

打开与关闭都有证据：`build/wire-palette-tool-out/open2.png`（真实鼠标压在瓦片上时展开面板里
中英文正常）与同一次运行日志里的 `Wire Palette: the expanded palette closed after the mouse left`
（鼠标移开后收起，`final-open.png` 是那一刻的画面）。

历史事故记录：为给展开界面做圆角而 `PushStyleVar`/`PopStyleVar`，触发游戏
`Assertion failed: Calling PopStyleVar() too many times!`（imgui.cpp:3758）。该段代码已删除；
这也说明注入点正处于游戏自己的样式变量作用域内（见 §5.1），插件**不要**动样式变量栈。

## 1. 接口（插件侧，已可用）

| 接口 | 位置 | 作用 |
|---|---|---|
| `tc::ui::registerBoardToolbar(slot_id, draw, user, host)` | `sdk/tc_ui.h` | 把一个控件注册进游戏工具栏；`draw(user, frame, availW, availH)` 在工具栏的窗口里每帧被调用 |
| `tc::ui::ToolTile` | `sdk/tc_ui_tool.h` | 按游戏瓦片度量画控件（80×80、圆角 10、底色 53/50/68、无绘图能力时降级为同尺寸按钮），并给出**几何 hover**、点击、原点（用于定位展开界面） |
| `tc::ui::panel` | `sdk/tc_ui.h` | 展开界面的推荐承载方式：普通窗口，圆角/样式跟游戏走，不需要动样式变量栈；插件传 `fontScale` 决定字号 |
| `tc::ui::HoverPopup` | `sdk/tc_ui_tool.h` | 悬停展开的生命周期（ImGui popup 版）：打开、贴瓦片旁边、跨缝隙宽限期 0.25 s、拖动不打断、离开两者才关。保留可用，但真机验证过的是普通窗口版（`examples/wire-palette`） |
| `tc::ui::closeCurrentPopup()` / `isWindowHovered()` | `sdk/tc_ui.h` | 上述实现需要的包装，已补齐 |

宿主侧契约（多 Mod 通用性）：

- 顺序按 `Mod id → 插槽 id` 排序后依次绘制，与加载顺序无关；日志
  `Board tools in the game's tool column (top to bottom): …`。
- 每个插槽在 `PushID(<mod id>)` + `PushID(<slot id>)` 作用域内绘制，多 Mod 同名控件不串。
- 每插件最多 8 个插槽；同名 `slot_id` 返回 −3，参数非法 −2，宿主过旧 −1。
- 只在关卡里绘制（离开关卡即停）。

## 2. 宿主实现（加载器侧）

- **注入点**：加载器 Hook 引擎的 `igEndChild`（MinHook，加载器独占），在返回地址等于
  `TC_BOARD_TOOLBAR_END_RVA`（`src/compat.hpp`，实测 `0x4659c1` = 游戏最后一个工具按钮那个
  子窗口的结束调用）时**先调用原函数、再绘制插件工具**——这样控件落在工具栏自己的窗口里，
  排在游戏工具之后。见 `src/loader.cpp` 的 `hookEndChild`。
- **网格**：加载器在每次 `igEndChild` 时记录工具栏内小窗口（32..200 px 见方）的屏幕矩形，
  学到列坐标、行距/列距（实测 96）；插件工具据此排到游戏工具的下一格，见
  `NativeRuntime::noteToolColumn` / `boardToolFrame`（`src/native.hpp`）。
- **字体（关键）**：注入点处当前字体是游戏的**图标字体**，插件文字因此一个字形都不产生。
  加载器在绘制插件工具前用游戏自己的字体表推送**正文字体**
  （`defined_fonts__presenterZimguiZimgui_u7413` +
  `igPushFont__presenterZimguiZimgui_u7614`），画完 `igPopFont()` 还原，见
  `NativeRuntime::resolveToolTextFont` / `boardToolFrame`。字号**不再**被改写：借用正文字体后
  窗口自身的缩放就已经给出游戏自己的 UI 字号（实测 45 px，与侧栏面板一致）。日志：
  `Tool column text font: defined_fonts[2]=NoroshiCode_Regular.ttf (used for plugin tool draws)`。
- 工具绘制失败会被隔离（`slot.failed`）并写日志，不会拖垮游戏。

## 3. 调色盘模组当前形态

`examples/wire-palette/plugin.cpp`：`drawTool` = `ToolTile` 画瓦片（当前颜色作为图标）+
悬停展开调色板内容（`取色器 / 使用此颜色 / 当前 ID / 游戏自带色 / 我的颜色 / 最近用过 /
新建颜色（可视取色器）/ 保存并选中`）。数据与行为（调色板表、取色器、新导线用当前色、
`palette.json`）未变。构建：`powershell -File build-wire-palette.ps1`（含单测）。

## 4. 实测事实（可直接复用）

| 项 | 值 | 来源 |
|---|---|---|
| 游戏工具瓦片 | 80×80、圆角 10、底色 `(53,50,68)`、悬停 `(68,64,86)` | 从真机截图采样 + 子窗口几何 |
| 工具网格 | 两列，x=366/462，y=174…666，列距/行距 96 | `igEndChild` 几何日志（`TC_MODLOADER_LOG_CHILD=1`） |
| 工具按钮注入点 | 最后一个瓦片子窗口结束的返回地址 `0x4659c1` | 同上 |
| 调色盘数据 | 导线记录 `+0x38` = 状态字节偏移、`+0x30` = 位宽 | 见 waveform-handoff §3.2 |
| 注入点当前字体 | `Icon_Complete.ttf`（size 72） | 探针 `tests/toolbar-font-probe.cpp` 读 `igGetFont` + `ImFont_GetDebugName` |
| 侧栏面板/主菜单页/帧尾字体 | `NoroshiCode_Regular.ttf`（size 45） | 同上 |
| 游戏字体表 | `defined_fonts[0]=Icon_Complete.ttf`、`[1]=NoroshiCode_Bold.ttf`、`[2]=NoroshiCode_Regular.ttf` | 同上，按符号 `defined_fonts__presenterZimguiZimgui_u7413` 枚举 |
| 游戏自己在工具栏里画标签的写法 | `igPushFont(2)` → `igPushFontScale(scale)` → `igText` → `igPopFont()` → `igPopFontScale()` | 反汇编 `build_function_icons__presenterZboard95uiZfunction95icons_u115`（0x465864…0x4659a1） |
| 注入点周围还有游戏自己的栈 | 我们的绘制点之后紧跟 `igPopStyleVar(1)` 与一次 `igPopFont` | 同上（0x4659c6 / 0x465a07） |

## 5. 已解决：展开界面里的文字不显示

现象：展开界面里的**矩形/色块/按钮底都能看到，唯独文字不显示**（中文、英文都一样）。

根因（实测，不是推断）：注入点被调用时，ImGui 的**当前字体是游戏的图标字体**
`Icon_Complete.ttf`；图标字体没有拉丁/中日韩字形，`igTextUnformatted` 提交的字符串因此
**一个字形顶点都不产生**，而矩形/按钮底不依赖字形，所以只有文字消失。

证据链：

1. 探针在三个上下文各读一次当前字体（`igGetFont` + `ImFont_GetDebugName`）：
   工具栏 = `Icon_Complete.ttf`，侧栏面板/主菜单页/帧尾 = `NoroshiCode_Regular.ttf`。
2. 反汇编游戏自己的工具栏代码：它画自己的标签时是
   `igPushFont(2)`（正则字体）→ `igPushFontScale` → `igText` → `igPopFont()` → `igPopFontScale()`，
   而**插件被注入的位置在这段字体作用域之外**（该函数随后还会 `igPopFont`，弹掉它自己的图标字体）。
3. 在探针里按同样的办法借字体后，工具栏里的文字立刻正常（`build/toolbar-font-out/shot.png`）。

修法（加载器，插件无需改动）：

- `NativeRuntime::resolveToolTextFont()`（启动时一次）：按符号取 `defined_fonts` 与
  `igPushFont__presenterZimguiZimgui_u7614`，优先选名字含 `Regular` 的正文面（本构建是 index 2）。
- `NativeRuntime::boardToolFrame()`：绘制插件工具前推送该字体、画完 `igPopFont()`，
  与游戏自己的字体作用域严格配对；**不再**改写窗口字号。

为什么"上次怀疑字号"是错的（保留教训）：

- 不是缺字形：同一批文字在**电路板侧栏面板**里显示正常（`registerBoardPanel` 上下文）。
- 不是绘图能力缺失：瓦片本身（画布绘制）正常，宿主也确认 `loadDrawing` 成功。
- 先前的两轮"字号"尝试（宿主设回菜单字号、插件设 `igSetWindowFontScale(1.0)`）都无效，
  因为问题根本不在字号：图标字体下"字号多大"都画不出拉丁/中文字形。
- 一度看起来"推了正文字体也没用"，其实是**探针只在每 120 帧画一次内容**，而截图只抓一帧，
  恰好抓到没画内容的那一帧；把探针改成每帧都画之后结论立刻反转。探针要每帧画。

## 5.1 注入点周围的坑（新增实测）

- 注入点位于游戏自己的**样式变量作用域内**：我们的绘制点之后游戏会 `igPopStyleVar(1)`，
  所以插件绝不能 `PushStyleVar/PopStyleVar`（这也是上次崩溃的原因）。
- 游戏在同一个函数里也持有字体作用域：之后还会 `igPopFont`。插件推送的字形必须**成对弹出**，
  乱弹会破坏游戏的栈。
- 图形绘制（画布/矩形）不受字体影响，所以"矩形在、文字不在"这种组合几乎一定先怀疑字体。

## 6. 验证与截图（开发用）

```powershell
# 1) 构建模组（含 palette 单测）
powershell -NoProfile -ExecutionPolicy Bypass -File build-wire-palette.ps1

# 2) 沙箱：只装"进入关卡"的探针 + 调色盘（不要装 board 驱动，它会抢占 handle_update_wire）
#    注意：从现用安装复制的沙箱要先还原 asset/shader/*.vert 再从 blobs 里 apply wire-palette，
#    否则着色器二次打补丁会让游戏启动即退出（见 verification.md「沙箱机制」）

# 3) 抓图：定时抓帧（菜单页的绘制只在主页运行，抓不到关卡内界面）
$env:TC_MODLOADER_SHOT='D:\shot\board.bmp'; $env:TC_MODLOADER_SHOT_DELAY='14000'
```

日志关键行：`UI slot registered: palette (board tool)`、
`Board tool local.wire-palette/palette drawn in the game's tool column (frame …, child …, at x,y)`、
`Board tools in the game's tool column (top to bottom): …`、
`Tool column text font: defined_fonts[2]=NoroshiCode_Regular.ttf (used for plugin tool draws)`。

更省事的一条命令（2026-09-18 新增，自带沙箱、驱动真实鼠标压在瓦片上、抓图并转 PNG）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/wire-palette-tool-playtest.ps1 -Name open
# 产物：build/wire-palette-tool-out/open.png（展开面板 + 文字）
```

它内部按 verification.md「沙箱机制」的顺序先还原 `asset/shader/*.vert` 再 `apply`，
所以不会二次打补丁；`-LeaveAfterMs 2000` 会让驱动在悬停 2 秒后把鼠标移开（关闭路径的
自动化验证受"沙箱与真人共用同一个鼠标/焦点"干扰，见 §9）。

## 7. 坑清单（都踩过）

1. **不要 PushStyleVar/PopStyleVar**：在这个注入位置配对会失衡，游戏直接断言崩溃。
2. **不要用 ImGui popup 承载文字**：popup 继承工具栏字号；且它盖住瓦片会让 item hover 消失
   → 弹窗逐帧"关→开"，表现为闪烁（已用几何 hover 修掉）。宿主现在会借正文字体，
   popup 里的文字也能画出来，但**验证过的做法是普通窗口**（`tc::ui::panel`）。
3. **弹窗与瓦片之间的缝隙**：鼠标慢速穿过时会被判为"离开" → 已加 0.25 s 宽限期。
4. **工具栏列很窄**（约 250 px）：插件控件必须按瓦片尺寸画，否则会挤掉游戏自己的布局。
5. **`handle_update_wire` 是独占的**：调色盘和 board 驱动/波形驱动抢同一个 Hook，沙箱里不能混装。
6. **探针要每帧画**：只在"每 N 帧"画一次的探针，会让定时抓帧抓到空帧，从而得出"文字没显示"
   的错误结论（这次就踩到了）。
7. **不要假设是字号**：先读 `igGetFont`/`ImFont_GetDebugName`。图形在、文字不在 = 先怀疑字体。

## 8. 相关文件

| 文件 | 角色 |
|---|---|
| `sdk/tc_ui.h` | `registerBoardToolbar`、popup/窗口包装、`closeCurrentPopup`、`isWindowHovered` |
| `sdk/tc_ui_tool.h` | `ToolTile`、`HoverPopup`（瓦片度量 + 悬停生命周期；文档里记了图标字体这条） |
| `src/loader.cpp` | `hookEndChild`（注入）、`noteToolColumn` 调用、定时截图 |
| `src/native.hpp` | `boardToolFrame`（排序、PushID、网格排布、借正文字体、失败隔离）、`resolveToolTextFont` |
| `src/compat.hpp` | `TC_BOARD_TOOLBAR_END_RVA` 等实测地址 |
| `examples/wire-palette/plugin.cpp` | 调色盘工具（`drawTool`） |
| `tests/enter-board.cpp` | 只进关卡的开发探针 `dev.enter-board` |
| `tests/toolbar-font-probe.cpp` / `tests/toolbar-font-playtest.ps1` | 字体探针：三个上下文的字体、`defined_fonts` 枚举、推字体前后的截图对照 |
| `tests/toolbar-hover-driver.cpp` / `tests/wire-palette-tool-playtest.ps1` | 进关卡 + 把真实鼠标压在瓦片上 + 抓图的端到端用例 |
| `tools/scan-calls.js` | 由 dll 导入表反查 EXE 里的调用点（找游戏自己的 `igPushFont/igText` 用法） |
| `build-wire-palette.ps1` / `tests/wire-palette.cpp` | 构建与调色板单测 |

## 9. 仍未验证／已知边界

- **关闭路径**：已观测到（日志行 + 收起后的截图），但它依赖真实鼠标真的离开：沙箱与使用者
  共用同一个鼠标和焦点，游戏窗口一旦失焦 ImGui 的鼠标位置就不再更新（几何 hover 会一直为真），
  所以这条不适合写成"必然通过"的断言。需要时用
  `tests/wire-palette-tool-playtest.ps1 -LeaveAfterMs 2000` 把鼠标主动移开，再在日志里找
  `Wire Palette: the expanded palette closed after the mouse left`。
- **多显示器／非 100% 缩放的工具栏**：本机只有一台显示器（175% 缩放的桌面），工具栏几何
  与瓦片命中在这些环境下未回归。
