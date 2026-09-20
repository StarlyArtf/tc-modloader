# 游戏风格控件与原生页面接入：交接（2026-09-17）

> **本文件是上一轮的计划。** 其中的“主菜单单页注册与返回”和“游戏外观按钮”
> 已在 [native-ui-pages.md](native-ui-pages.md) 落地，且实施过程中发现了本文没有
> 预料到的 `ImVec2` sret ABI 问题（本文记录的 `mouse=0,0` 证据就是它的症状）。
> 请与那份文件一起读。

## 本轮状态

用户要求只做一小步并详细交接。本轮添加 `sdk/tc_ui.h` 的 `tc::ui::Id` RAII 作用域，
为多个插件共享原生页面提供控件 ID 隔离。它成对检查 push/pop，禁止复制和移动，
离开作用域自动 pop。`tests/ui-id.cpp` 已编译并通过：未加载、部分绑定、空 ID、
嵌套和异常展开。这只验证栈管理，不代表视觉或输入验收。

**尚未实现**游戏风格按钮、页面注册或运行时 UI 插槽。本轮没有改游戏安装、没有
重打安装器。Id 测试单独编译，未加入 build.ps1。现有 on_frame 面板仍走原路径。

```cpp
// 未来宿主在已有窗口内包围插件页面的绘制回调。
tc::ui::Id modScope("author.mod");
tc::ui::Id pageScope("settings");
tc::ui::button("Apply");
```

## 已确认的工程事实

- 仓库 `D:/p/tc-modloader`，游戏在 `D:/p`；Windows x64，MinGW 默认
  `C:/msys64/ucrt64/bin`，仅适配 2.1.334 指定哈希。
- tc_ui.h 使用游戏的 cimgui/ImGui 上下文，经 host->engine_proc 获取导出。
- src/loader.cpp 的 igInvisibleButton 代理记录 homeSeen。igEnd 代理在菜单结束
  调用点且 homeSeen || managerOpen 时，先调用 draw()，再调用原 igEnd。
  因此当前 Mods 入口绘制于**游戏已有菜单窗口内**。
- NativeRuntime::frame()/插件 on_frame 在原 igEnd **之后**执行，不能当作原窗口
  内容插槽。页面内绘制需要独立调度入口。
- src/compat.hpp：菜单结束调用点 RVA `0x456c33`，主页探测范围
  `[0x449df0,0x44b610)`；这些是锁定构建的内部细节，不能暴露给插件。
- 主菜单函数 `build_main_buttons__presenterZmain95menu95uiZhome95page_u48`
  起点 VA `0x140449df0`。反汇编确认使用字体缩放、igCalcTextSize 和 igInvisibleButton；
  例如 VA `0x14044a122` 调 igInvisibleButton。点击分支随后调用
  `play_sound__presenterZutilities_u29270`、`change_scene__presenterZcontext_u2958`。
  这不是可直接复用的通用按钮函数，它混有业务副作用。
- 游戏存在 presenter/renderer/multi_mesh/component_button_mesh 系列符号。
  不能因为主菜单使用 ImGui，就断言电路板、图标和全部 UI 外观都由 ImGui 绘制。
- COLOR_BUTTON_*、COLOR_TEXT_* 符号可枚举，但其数据布局与消费者**未验证**。
  不要直接强转为 ImVec4；需要核实 float/double/打包颜色。

## 下一轮最小闭环

### 1. 主菜单单页注册与返回

先只接主菜单，不同时动电路板侧栏。沿现有 Mods 入口路径，让测试插件注册页面，
主菜单出现入口，点击打开，返回后仍在主菜单；进入关卡后不继续绘制该页。

建议 TCHost 尾追加可选 register_ui_page，参数为带 size 的 C 结构，含页面本地 ID、
标题、绘制回调、user。**这是设计建议，尚无对应 ABI。** SDK 检查 host.size 和函数
指针，使旧加载器安全降级；不要扩充 tc_ui::load 的必需符号集合而破坏旧 UI 插件。

- 仅 tc_mod_load 注册，宿主复制 ID/标题，按 mod_id 命名空间拒绝重复 ID。
- 注册先暂存，插件成功后激活，失败清除；只绘制 active 插件，不支持热卸载。
- 宿主负责容器、标题、返回按钮及 mod_id/page_id 两级 Id；插件只画内容。
- 明确坐标空间、可用内容区域、弹窗、关闭协议；禁止插件结束宿主窗口。
- 不暴露未经核实的游戏对象指针。异常禁用故障页，同时保留返回路径。
- 当前 boot 在 frame() 内，即菜单 igEnd 后，首帧可能尚无页面注册。
  允许第二帧出现，不要为首帧显示而搬动初始化时机。
- 新插槽与 on_frame 分别去重；不能共享 lastFrame 导致其中一个被跳过。

### 2. 一个真正匹配游戏外观的按钮

先截图采集普通、悬停、按下、禁用状态，记录字体、配色、间距、点击区域、缩放。
沿主菜单反汇编确认背景与文字来源，再决定复用最小绘制原语还是提取样式。

建议独立可选 tc_game_ui.h，保留 tc_ui.h 的通用工具用途。调用游戏内部函数前必须
验证全部参数、返回 ABI、Nim 字符串所有权、错误标志及上下文依赖；名称不是签名
证据，不要调用带切场景逻辑的整段主菜单函数。

style/font/ID 栈全部成对恢复，不遗留全局主题修改。0.62 只是当前插件面板字号，
不是游戏统一缩放规则。保留 disabled、焦点、键盘导航和点击语义。

### 3. 板上面板插槽

候选符号（只确认存在，签名和安全 Hook 位置未验证）：

- `build_bottom_panel__presenterZboard95uiZbottom95panel_u253`
- `build_buttons__presenterZboard95uiZmenu95bar_u187`
- `build_edit_button__presenterZboard95uiZbottom95panelZuser95defined95ui_u2587`

沿调用者确认 Begin/End、板上下文、选择变化、裁剪区域，再确定 board.toolbar 或
component.inspector 插槽。先做只读面板。Hook 由加载器统一调度：现有同目标只能
安装一个 Hook，不能让各插件竞争安装。

## 验收与注意事项

1. 使用隔离游戏副本和独立 USERPROFILE/APPDATA，参照 tests/ui-playtest.ps1。
   后台进程用 -WindowStyle Hidden，不修改用户电路和原版存档。
2. 测试至少覆盖两个插件同名按钮互不干扰、非法/重复注册、插件失败后无残留入口、
   关闭重开、进出关卡、旧宿主降级、每帧不重复绘制。
3. ui-playtest 的帧日志只能证明画过。正式验收还需截图和鼠标操作，检查样式、
   点击、裁剪、弹窗、DPI 和窗口尺寸变化。
4. tc_ui.h 的物理热键不等价于 ImGui 输入捕获；失焦、文本输入中、Esc 返回、
   板上快捷键穿透需单测。
5. 捕获 C++ 异常不能自动修复不平衡的 Begin/End 或样式栈。提供 RAII 和绘制协议，
   不承诺任意坏插件都能恢复。Id 只隔离命名，不隔离权限和全部 UI 状态。
6. Windows x64 下 Vec2、Vec4、结构返回值逐个核实。igSetNextWindowPos 必须传第三个
   pivot 的历史缺陷就是例子；不要混用 Nim 和不同 ImGui 入口的签名。
7. 工作区有大量既有未提交/未跟踪改动，不要 reset/clean。上一轮声明式元件已完成
   和构建，需要保留。本轮仅修改 tc_ui.h，新增 ui-id.cpp、本文及 UI 文档链接。

## 复现命令

```powershell
cd D:/p/tc-modloader
node tools/xref.js callers igInvisibleButton
node tools/xref.js dis build_main_buttons__presenterZmain95menu95uiZhome95page_u48
& C:/msys64/ucrt64/bin/g++.exe -std=c++17 -O2 -static tests/ui-id.cpp -o build/ui-id-test.exe
./build/ui-id-test.exe
# 运行时接口完成后再构建和实机验收：
./build.ps1
./tests/ui-playtest.ps1
```

先读 sdk/tc_ui.h、sdk/tc_mod_api.h、src/native.hpp、src/loader.cpp、src/compat.hpp；
示例看 examples/mod-inspector/plugin.cpp。

接手首个任务：**实现主菜单单页注册/返回闭环，用两个测试插件验证 ID 与生命周期，
然后完成一个经过截图比对的游戏风格按钮。**
