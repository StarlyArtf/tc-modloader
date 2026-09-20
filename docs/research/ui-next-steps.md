# UI 接口逐项推进（2026-09-18）

## 0.1 2026-09-18 第三轮：工具栏里的插件文字修好

上一轮留下的唯一未决问题是"工具栏插槽展开界面里没有文字"。根因查清并修好了：注入点处
ImGui 的当前字体是游戏的**图标字体**（`Icon_Complete.ttf`，没有拉丁/中日韩字形），
所以文字一个顶点都不产生，而矩形/按钮底照画。加载器现在按符号读游戏自己的字体表
（`defined_fonts__presenterZimguiZimgui_u7413`），挑出正文面（本构建
`defined_fonts[2]=NoroshiCode_Regular.ttf`），在绘制插件工具前用游戏自己的
`igPushFont__presenterZimguiZimgui_u7614` 推送、画完 `igPopFont()` 还原，并不再改写窗口字号
（借用正文字体后就是游戏自己的 UI 字号，实测 45 px，与侧栏面板一致）。插件侧零改动即可显示
中英文。

另外两条这一轮踩出来的教训：探针必须**每帧**画内容（只在"每 N 帧"画的探针会被定时抓帧抓到
空帧，从而误判"文字没显示"）；判断这类问题先读 `igGetFont` + `ImFont_GetDebugName`，
不要先猜字号。证据、反汇编与端到端用例见
[toolbar-tool-handoff.md](toolbar-tool-handoff.md) §5 与 `docs/verification.md`。

## 0. 2026-09-18 第二轮：板侧栏插槽已实现并验证

上一版这一节写的"已定位，尚未实现"已经落地，做法与当时的推断不同，记录如下：

- 通道不是 MinHook，而是**加载器自己实现 `igIsAnyItemActive` 导出**。EXE 从
  `game_engine.dll`（即加载器）导入 `igEnd`/`igInvisibleButton`/`igIsAnyItemActive`，
  所以加载器在这个导出里就能拿到调用者返回地址、在游戏采样前插入绘制。
  `tools/exports.js` 相应把 `igIsAnyItemActive` 从"转发"改为"自己实现"。
- 采样点是 `build_board_ui` 的 `0x46b58e` 调用（返回 RVA `0x46b593`）；紧随其后
  `0x46b5ae` 的 `igIsWindowBgActive` 与 `0x46b5e2` 的 `igIsWindowHovered`
  （注意 hover 的返回地址是 `…5e2`，不是 `…5e3`）共同组成交给 `handle_io_on_board`
  的标志对。面板占用鼠标时把 `igIsAnyItemActive` 回答成 true，游戏自己就不会把这一帧
  的鼠标当成板面输入。
- **MinHook 会跟进 `jmp` thunk**：插件若 Hook 同一个入口，被钩函数看到的返回地址变成
  插件 DLL 里的地址。加载器因此增加短线栈回溯（`RtlCaptureStackBackTrace`）作为回退。
  这是本轮踩到的最贵的坑，见 [验证体系](../verification.md)。
- 沙箱必须每次刷新 `game_engine.dll`，否则会用上一版加载器静默跑测试（同上）。
- 结果：`sdk/tc_ui.h` 的 `tc::ui::registerBoardPanel()`、宿主 `register_ui_slot`、
  示例 `example.board-panel`、真机用例 `tests/ui-board-panel-playtest.ps1`
  （真实点击 + 正对照 + 场景关闭）、离线 `tests/ui-slot.cpp`。
- 插槽内容后来补上了裁剪与滚动：内容画在独立子窗口里，超出部分不可悬停／点击；
  右缘滚动条由宿主绘制并驱动（拖真实鼠标即可滚动，加载器日志给出权威滚动量）。
  同一轮发现本构建的鼠标滚轮不会送到 ImGui（内容子窗口本身可滚动，`ScrollMax` 正常），
  所以滚轮滚动不做承诺。
- 随后把同一段内容区实现用到主菜单页面容器：页面内容也裁剪／可滚动，
  `tests/ui-page-playtest.ps1 -DriverMode` 断言内容区与滚动条存在、页面控件仍可点击；
  驱动包的 id 也从 `dev.menu-demo` 改为 `dev.menu-demo-driver`，否则 `make-ui-sandbox`
  按文件名找不到它。
- 键盘做了一轮实测：`tc_ui.h` 现在暴露 `keys::*`（ImGuiKey 值经真实按键消息核对）、
  `setKeyboardFocusHere()`、`isItemFocused()`；探针包 `dev.ui-keyboard-probe` +
  `tests/ui-keyboard-playtest.ps1` 用真实 `WM_KEYDOWN/UP`、`WM_CHAR`、`WM_IME_CHAR`
  验证按键与字符（含 `你`/`好` 的 UTF-8）都能进入插件控件，Escape 先被输入框吃掉。
- 同一轮先量出一个"缺陷"又推翻它：第一版驱动同时发 `WM_KEYDOWN` 和 `WM_CHAR`，
  每字符出现两次；原因是消息循环的 `TranslateMessage` 会把按键转成 `WM_CHAR`，多发
  的那条才是重复来源。改为只发按键消息（真实键盘路径）后逐段断言缓冲区内容：
  `"" → "abc" → "abc你好" → "abc你好z"`，一次一个字符，**不需要插件去重**。
- 中文输入法与板上打字经人工验收通过（2026-09-18，用户确认）：候选窗跟随光标
  （`IME board panel composition/candidate style=0x32` 随每个字移动）、中文以 UTF-8 进入
  缓冲区、板上打字未触发板面快捷键。唯一已知现象是独占全屏下候选窗出现时画面闪一下，
  属系统合成行为。
- 显示这一块也推进了一步：加载器每次启动记录 `Display:`（DPI 感知模式、窗口 DPI、
  显示器数量、客户区／画布尺寸与比值），并新增"改窗口尺寸后能否点中"的自动断言
  （2560x1600 → 1792x1120，面板重排、坐标更新、点击仍命中 `count=2`）。本机是
  175% 缩放（`window-dpi=168`）的桌面，因此这些断言本身就在缩放环境下通过。
  顺带去掉内容子区域里那条不可交互的引擎滚动条，插件拿到完整宽度。
- 面板折叠也做完了：宿主在标题行画 `-`/`+` 开关，折叠后只剩标题栏；状态归宿主，
  折叠期间内容不显示也不可点、但插件 `draw()` 仍会被调用。真机断言：折叠后矩形高 60、
  折叠期间点按钮不计数、展开后恢复。
- 仍未做：把窗口拖到另一台不同缩放的显示器上验证（本机只有一台显示器，用户已确认没有
  第二台，作为已知空白保留）。

## 1. 图片／纹理：已实现与回归

- `sdk/tc_ui_texture.h`：可选能力加载、不可复制的 `Texture`、图片文件／RGBA、画布图片与图片按钮。
- `TCHost` 尾部追加 create/load/release；旧宿主按 size 检测降级，旧插件 ABI 保持。
- `src/ui_texture.hpp`：WIC 从包内文件字节解码（支持中文路径），GL 上传；恢复绑定、
  unpack 参数和 PBO；每 Mod 限额和独立句柄；释放延迟到下一 ImGui 帧；初始化失败后清理。
- `ImDrawList_AddImage` 在此引擎仍接收 64 位纹理 ID；入口 VA `0x180026d20`，
  与当前 draw command 内部使用 16 字节 TextureRef 不同，不能混用两种参数类型。
- 单测、全量构建、`tests/native.ps1`、`tests/ui-texture-playtest.ps1` 通过。
  真机测试回读 RGBA／透明度／方向、UV 顶点、GL/PBO 状态，测试线程限制、错误路径、
  损坏图片、句柄所有权、配额、重复释放和延迟销毁，并连续绘制 180 帧。
- `example.drawing-demo.mod` 包含 PNG 示例，新增翻转／重载／释放按钮。
  安装器与 loader 在 dist/ 已重建；尚未替换 D:/p 的现用游戏 DLL。

## 2. 原生板界面嵌入：已定位，尚未实现

下一步应接一个真实的原生容器插槽，并单独验证输入与场景生命周期，不能只按上一帧
出现过 board 标记就假定当前仍是电路板。

本次读到的锁定构建调用链：

- `build_menu_bar__presenterZboard95uiZmenu95bar_u1185` VA `0x14045cb60`。
  `igBeginChild_Str` call `0x14045cde1`，原生工具栏按钮 call `0x14045ce1c`，
  `igEndChild` call `0x14045ced1`（返回地址 RVA `0x45ced6`）。
- `build_bottom_panel__presenterZboard95uiZbottom95panel_u253` VA `0x1403cad30`。
  此函数也被关卡树调用，不能据此单独判定 board-only 插槽。
  原生底栏 BeginChild call `0x1403cb2e9`，EndChild call `0x1403cb2f6`。
- `build_board_ui__presenterZboard95ui_u15` 调底栏 `0x14046b4a8`，随后在
  `0x14046b58e` 调 `igIsAnyItemActive`、`0x14046b5dd` 调 `igIsWindowHovered`，
  **最后** `0x14046b5e9` 才调 `igEnd`（返回 RVA `0x46b5ee`）。
  这些输入结果会继续用于游戏逻辑；若在 igEnd 内才插入可交互控件，输入结果已被读走，
  有点击穿透风险。需要在原生输入采样之前插入，而不是直接沿用浮动 on_frame 的时机。

只做过静态调用链检查，尚未验证上述入口的运行期几何／可用空间／场景切换，
也没有增加新的 Hook 或宣称插槽已经可用。

## 后续顺序

3. 焦点与输入：键盘导航、快捷键穿透、中文 IME、画布真实鼠标拖动。
4. DPI／缩放：视口原点、系统 DPI、游戏 UI 比例，变化与多显示器回归。

所有真机测试继续使用独立游戏与 USERPROFILE/APPDATA。工作区有大量前序未提交内容，
不可 reset/clean；已有绘图、元件和原生逻辑成果须保留。
