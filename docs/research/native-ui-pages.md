# 主菜单页面接入：交接（2026-09-17，第二轮）

> 2026-09-18 更新：下文“draw list 不能从插件调用”是旧探针失败后的推断，已被
> 新的真实引擎测试推翻。`sdk/tc_ui_draw.h` 提供经过类型化 ABI 验证的画布；
> `tests/ui-draw-playtest.ps1` 验证所有图元、顶点坐标／颜色、滚动与裁剪，连续 180 帧。
> `igGetCursorScreenPos(Vec2*)` 也已实测可用。旧失败的具体触发原因没有足够证据确认。

上一轮的 [native-ui-handoff.md](native-ui-handoff.md) 提出“主菜单单页注册与返回”
加“一个游戏外观按钮”。本轮完成这两件事，并把没做完的部分写清楚。

## 本轮改了什么

| 文件 | 改动 |
|---|---|
| `sdk/tc_mod_api.h` | `TCHost` 尾部追加 `register_ui_page`；新增 `TCUiPageDefinition`、`TC_HOST_BASE_SIZE` |
| `sdk/tc_ui.h` | 新增 `Id`、`registerPage()`；**修 ABI**：`igGetMousePos` 改成 out 指针；去掉两个必需符号 |
| `sdk/tc_game_ui.h` | 新增：游戏配色常量 + `menu_button` / `framed_button` |
| `src/native.hpp` | 页面注册表、页面容器绘制、`uiFrame()`、板场景标记 |
| `src/loader.cpp` | 主页页面入口、页面调度、`home button` 矩形日志 |
| `src/core.hpp` | `ui_scale`：记录主菜单当前缩放，供页面容器对齐 |
| `examples/menu-demo/` | 测试插件（同一份源码编出两个包：`dev.menu-demo`、`dev.menu-demo-peer`） |
| `tests/make-ui-sandbox.ps1` | 复用式隔离沙箱 |
| `tests/ui-page-playtest.ps1` | 注册／绘制／生命周期验证（`-ProbeButtons`、`-DriverMode`、`-OpenSecondPage`） |
| `tests/ui-page-driver.hpp` | 进程内驱动：点主页入口、点页面按钮、报出页面几何 |
| `docs/sdk/ui.md`、`docs/verification.md` | 文档与人工验收步骤 |

## 本轮最重要的发现

**`ImVec2` 不是按值返回的。** 把 `igGetMousePos` 当“返回两个 float”调用时，
先得到 `0,0`、再得到 `-2147483648`；改成 out 指针后立刻返回真实坐标 `2392,164`。
这意味着 `tc_ui.h` 里所有 `Vec2` 返回值都要按 out 参数声明。
顺带解释了为什么旧文档里记的 `mouse=0,0` 证据其实是坏的。

**draw list 能用，但不能从插件调。** `ImDrawList_AddRectFilled` /
`ImDrawList_AddText_Vec2` 在本构建导出表里存在，但在插件的绘制回调里调用会卡死渲染
线程：第一帧之后游戏不再出帧（日志里能看到帧号停在页面首帧）。用
`igPushStyleColor_Vec4` + `igButton` 就没有这个问题——主菜单自己也是这么做的。

## 怎么验证的

```powershell
./build.ps1                                   # 全量构建 + 既有全部单测
./tests/make-ui-sandbox.ps1                   # 隔离副本（独立 USERPROFILE/APPDATA）
./tests/ui-page-playtest.ps1 -ProbeButtons    # 记录主页按钮矩形
./tests/ui-page-playtest.ps1 -DriverMode      # 打开页面、画出来、报几何
./tests/ui-page-playtest.ps1                  # 两个插件注册同名页面
./tests/native.ps1                            # 原有原生加载测试
./tests/ui-playtest.ps1                       # 原有面板测试
```

日志里的关键行（都是真机运行的真实输出）：

```
[dev.menu-demo] UI page registered: settings
[dev.menu-demo] Menu demo: 注册页面 'settings'
Opened UI page dev.menu-demo/settings
[dev.menu-demo] Menu demo: page 'settings' first drawn on frame 124, content 2560x1497
[dev.menu-demo] DRIVER: Apply item size 128x51
[dev.menu-demo] DRIVER: mouse 2392,164 overApply=0
```

`[tcmod.mod-inspector] Mod Inspector: panel drawn; viewport=2560x1600 mouse=1954,784`
——ABI 修复后鼠标坐标终于合理，这条旧测试也顺带变强了。

## 没做到的，以及为什么

### 1. 页面容器内的点击：找到并修掉一个真实缺陷

最初以为"容器内收不到鼠标输入"。**那个结论是错的**，但它把真正的缺陷逼了出来：

容器窗口实际只有 **32×32**（ImGui 的最小窗口尺寸）。宿主把
`igSetNextWindowSize` / `igSetNextWindowPos` 声明成了指针参数，而本构建要求**按值传参**
（加载器自己的 Mods 弹窗就是按值调用且尺寸正常）。签名不匹配后两个调用静默失效，窗口
退化为默认大小，页面内容全被画到窗口外面——用户看到的就是"页面出现了，但里面什么都点
不到"。

改成按值之后：

```
DRIVER: container window size 2560x1600
DRIVER: full-region probe itemHovered=1 windowHovered=1
DRIVER: CONFIRMED - a click reached a widget inside the page container
```

全屏 416 个采样点全部 hover 到容器，控件级 hover 与点击都成立。

诊断过程中还有两个坑值得记下来：

- 用 `SetCursorPos` 把光标摆到精确坐标来点按钮，在这台机器上不可靠——另一个程序
  （用户同时开着别的游戏）会持续把光标拉回屏幕中心，驱动设的位置立刻被覆盖，扫描结果
  时好时坏，差点又被误读成"输入不通"。现在的测试改成页面自己放一个覆盖内容区的探测
  按钮：只要光标落在页面内任意位置就会被 hover、被点击，完全不依赖光标放置。
- 坐标空间：页面用 `cursorPosX/Y` 上报的是**窗口内**坐标（实测按钮在 `8,204`，尺寸
  `111x51`），而游戏读的鼠标是屏幕坐标，两者差一个窗口位置，而 `igGetWindowPos` 在本
  构建不可用，拿不到这个偏移。

`tests/ui-page-playtest.ps1` 现在断言容器尺寸、控件 hover、点击到达控件。
2026-09-18 又人工走了一遍完整交互（打开页面、点 Apply、返回、打开另一个插件的同名
页面、进关卡不残留），全部通过，记录在 [docs/verification.md](../verification.md)。

### 2. 游戏外观按钮：不做并排截图比对了

颜色和布局有据可查（反汇编 + `.rdata` 常量），但沙箱里拿不到该窗口的帧缓冲：
`glReadPixels` 从插件里读永远是黑（前后缓冲都试过），`PrintWindow` 只得到背景色。

人工看过之后决定**接受当前观感，不要求与游戏原生 UI 完全一致**，这项关闭。颜色常量
列在 `tc_game_ui.h` 头部，若以后想更贴近可直接对照调整。

### 3. `igGetItemRectMin/Max`、`igGetCursorPos`、`igGetWindowPos` 不可用

这四个导出存在，但按 out 指针调用返回 `0x80000000`。已经在 `tc_ui.h` 里刻意不暴露，
只在注释里记录，免得下一个人再踩一次。可用的只有 `igGetMousePos` 与
`igGetItemRectSize`。

### 4. 板上面板插槽没动

按上一轮计划，本轮只接主菜单。板侧栏（`build_bottom_panel__...` 等）仍然只用于
场景判定：加载器在 boot 时解析它们的地址范围（只存地址，不暴露游戏对象指针），
`igInvisibleButton` 命中这些范围时把页面关掉。"进入关卡后页面不残留"已由人工验收
确认；板侧栏本身的只读面板插槽仍未实现。

## 下一步建议（按优先级）

1. 补板上面板的只读插槽（`board.toolbar` 或 `component.inspector`）：先沿调用者确认
   Begin/End、板上下文、选择变化与裁剪区域。
2. 页面容器的裁剪与滚动（目前没有，超出高度的内容会画到区域外）。
3. 页面容器内的键盘导航与 IME 输入未测。
