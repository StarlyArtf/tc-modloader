# 交接：选中提示（白色弧线）改为跟随 footprint

> **2026-09-23 已收口（先读这一段）。** 下面 §2 的"下一轮第一件事"已经做完，且 §3 的推断有一处
> 需要更正，结论如下：
>
> 1. 钩子已改挂 `redraw_selection__presenterZupdate95state95common_u5104`，`focus_components` 的钩子
>    与其 trace 代码已删除（`armFocusSuppression` 改名 `armSelectionHintControl`）。
> 2. **§3 的更正**：白弧不是"一个固定缩放的圆环实例"那么简单，也不是按元件网格画的——`redraw_selection`
>    遍历的是 presenter 选中容器里的**两个哈希集**（`+0x00` = selected_components、`+0x18` =
>    selected_wires），桶宽 0x20，桶 `+0x08` 是占位字（循环判空用）、`+0x10` 是元素 id（元件侧 =
>    **元件序列索引**）、`+0x18` 是圆环缩放。占位字非 0 就为该元素加一个 selection 精灵实例。
>    所以正解是"**按元素把占位字在绘制帧里清 0**"：该元件根本不产生实例，其它元件/导线不受影响。
>    归属不靠循环序号猜：用游戏自己的 `contains(selected_components, id)` 判真、`contains(selected_wires,
>    id)` 判假，索引空间与 `dispatchComponentRender()` 走的元件序列一致。
> 3. **§4 第 3 步的"改 packed 变换"不需要了**：不存在"increment 已经占号"的问题，因为清占位字之后
>    循环压根不进那一次迭代。
> 4. **§4 第 4 步的范围证据**已经进脚本断言：`TC_TEXTBOX_KEEP_SELECTION=1` 场景里，被抑制便签的窗口
>    **0** 白像素，且截图那一帧它仍然是被选中的那一个（`selected_at_capture=1`）；
>    `TC_TEXTBOX_KEEP_SELECTION=1 TC_TEXTBOX_KEEP_ARCS=1`（不调 `set_selection_hint` 的灵敏度对照）
>    同一窗口 **546**。默认门禁另加两条日志断言（`selection hint control armed`、
>    `cleared the game's selection ring for N Mod component(s)`）。**做不到的部分**：同一帧里同时看到
>    一个被抑制和一个未被抑制的元件环——游戏里再按一个元件是替换选中，Shift-click 与框选在本棋盘
>    状态下都不加选（都试过）。
> 5. 门禁：`./build.ps1`、`./tools/abi.ps1`（1192 条不变）、`./tests/text-box-playtest.ps1` 默认运行全绿；
>    弧线场景命令见 `docs/verification.md` 的「游戏自己那圈白弧已按类型关掉」。诊断开关是
>    `TC_SELECTION_TRACE=1`（新的；`TC_FOCUS_TRACE` 已随死靶删除）。
> 6. `build-text-box.ps1` 补了 `SOURCE_DATE_EPOCH`（与 `build.ps1` 同一个默认值），同一个源码连编两次
>    得到同一个 `.mod` 哈希；部署后的两份文件与 `dist\` 里沙箱跑过的逐字节相同，可直接用哈希复核。
> 7. **原版那圈的几何已经量清楚，但模仿没做进示例**：圆心 +1.00 格、内缘半径 0.80 格、线宽 0.21 格、
>    两段弧 44°–137°/223°–315°、随板面缩放——都在 `docs/verification.md`「模仿原版那圈提示」，
>    并按用户决定登记为**未使用能力**（[reference/unused-interfaces.md](reference/unused-interfaces.md)
>    第 1 条，含参数、对拍数字与接回来的步骤）。便签的提示保持"跟着 footprint 的矩形"。
>
> 残留在 §8 的其它结论不受影响；本文下面的原文保留作过程记录。

日期：2026-09-23（未完成，做完前面 5 步即可收口）
工作区：`D:\p\tc-modloader`；真机沙箱与产物在 `build/`
上游背景：见 [HANDOFF-component-m5.md](HANDOFF-component-m5.md)（M5 棋盘绘制），本文只讲"选中提示"这一条线。

## 0. 需求与已确认的边界

用户原话与结论：

1. **碰撞箱 = footprint 是对的，不用分离。**（一般元件的碰撞箱本来就该和形状一致；`set_footprint` 写的就是
   原型那条局部矩形序列，引擎里放置/占位、走线避让、presenter 快照都读它，见
   [research/component-hitbox-path.md](research/component-hitbox-path.md) 的可复用结论。）
2. **选中提示（里面那圈白色弧线）必须跟随 footprint**，现在它只跟着游戏给自定义元件用的网格大小
   （实测约 3.9×3.2 板面格，而便签 footprint 是 8×4）。← 本文要解决的就是这一条。

## 1. 已经做完并且验证过的部分（不要重做）

| 内容 | 状态 | 证据 |
|---|---|---|
| `tc.component.render` **V3**：`set_selection_hint(context, custom_id, enabled)` | 已交付 | `sdk/tc_service_api.h`、`sdk/tc_component_render.h`（`tableV3`/`setSelectionHint`）、`src/native.hpp` 的 `component_render_set_selection_hint_api`、`tests/component-render.cpp` |
| ABI 基线 | 已更新到 **1192** 条 | `tools/abi.ps1` 通过；新增记录全属 `TCComponentRenderApiV3`（`sizeof=40`，V1/V2 一条未变） |
| 便签自绘的**白色** footprint 提示环（选中时 ±4/±2 板面格 + 淡填充） | 已实现并验证 | `examples/text-box/plugin.cpp` 的 `renderNoteOutline`；门禁断言 `PASS text-box selection hint follows footprint width=225.3px height=112.6px unit=28.16 rotation=0`（外接框 = 8×4 格、框心 = 元件中心） |
| 选中状态读取 | 已实现 | 走 `sdk/tc_board_model.h` 的 `tc::TCBoardModel`（键 = **元件序列索引**），因此 `NoteOnBoard` 增加了 `index` 字段；`frame()` 每帧刷新 `selectedInstances` |
| 文档 | 部分完成 | `docs/verification.md`「选中提示跟随 footprint」一节、`docs/changelog.md`「选中提示按 footprint 画」条目已写；**但都只写了"Mod 自绘提示"，不要把它们读成"游戏白弧已关掉"** |

## 2. 没做完的部分：游戏那圈白弧还在

宿主侧的钩子目前挂在**错误的函数**上，所以 `set_selection_hint` 是空操作：

- `armFocusSuppression()` 挂在 `focus_components__presenterZupdate95state95common_u5084`（`src/native.hpp`）。
  带 `TC_FOCUS_TRACE=1` + "前 5 次调用无条件记录"跑完整轮棋盘运行，**一条 trace 都没有**，说明这个函数在
  棋盘阶段根本不被调用（早先看到的 3 条记录来自主菜单阶段）。
- 同一轮截图里白弧照旧：footprint 内缩窗口白像素 **543**；不开 `set_selection_hint` 的对照运行是 **541**
  （窗口与量法见 §5）。两者没有差别 → 白弧没有被关掉。
- 结论：`focus_components` 是死靶。**下一轮第一件事就是把它改挂到下面的 `redraw_selection`**（或者删掉，
  别让它继续在日志里声称"selection hint control armed"）。

## 3. 真正的 pass：`redraw_selection__presenterZupdate95state95common_u5104`

反汇编（`build/exe-disasm.txt`，pinned 构建 2.1.334）与调用点如下：

- 开头一次 `clear_all_instance__presenterZrendererZmulti95meshZselection95mesh_u469`。
- 两轮循环：元件的选中集 `len__modelZboardZboard_u16536`、导线的选中集 `len__modelZboardZboard_u16858`；
  中间有 `is_placing_wire__presenterZutilitiesZhelper95functions_u6879` 守卫。
- 每个被选中元素依次：

  ```text
  initTransform2D__presenterZlevel95tree95ui_u232
  toPackedTransform2DSameScaleNoRotation__presenterZrendererZtransform952d_u139
  increment_instance_count__presenterZrendererZmulti95meshZselection95mesh_u713
  set_transform2d__presenterZrendererZmulti95meshZselection95mesh_u727(mesh, &packed, index)
  ```

  其中 `index` 来自 `mov 0xbc(%rsp),%eax; lea -0x1(%rax),%r8d`，即该元素在循环里的序号（从 0 起）。
- **为什么它不可能"跟着 8×4 变"**：函数名里的 `SameScaleNoRotation` 说明这个圆环实例是**同一缩放、无旋转**，
  缩放来自 `level_tree_ui` 的固定上下文，与元件的 footprint、旋转、宽高比都无关。即使改写变换，也只会得到
  一个更大的**圆**，不是 8×4 的矩形。
- 因此正确路线只有一条：**按元件关掉游戏这个圆环，由 Mod 按 footprint 画**（后者已完成，见 §1）。

## 4. 下一轮的具体动作（做完这五步就收口）

1. `armFocusSuppression()` 改挂 `redraw_selection__presenterZupdate95state95common_u5104`，并删掉
   `focus_components` 的钩子与其 trace 代码。
2. 认出"这一次循环是不是我们的元件"。两个候选判据（二选一，先各跑一次定性）：
   - ① 钩 `initTransform2D__presenterZlevel95tree95ui_u232`，读它的 vec2 位置参数，与宿主按序列枚举到的
     元件坐标（`board_objects` 的 +0x02/+0x04）比对；
   - ② 用循环序号 `index` 与选中集对齐——选中集是按**元件序列索引**存的，宿主本来就按序列走。
3. 命中之后**不要跳过** `set_transform2d`：`increment_instance_count` 已经为它占过号，跳过会让该实例退回
   默认变换。正确做法是传一份"缩放到 0 / 平移到屏外"的 packed transform 副本。**先 dump 一次 packed 的
   字节布局**（`toPackedTransform2DSameScaleNoRotation` 的输出，栈上 0xa0(%rsp) 那一块）再动手。
4. 沙箱 A/B（同一条件、同一量法）：
   - 便签/隐藏探针被选中 → 白弧像素 ≈ 0；
   - **同一轮**里让一个非抑制对象被选中（内置元件，或把 `DEFAULT VISIBLE` 探针改成也选中）→ 白弧像素仍有数百。
   这一条是"作用域正确"的硬证据，不能省。
5. 跑一次完整门禁（`tests/text-box-playtest.ps1`，见 §6），通过后再：部署到真机（见 §7）、
   更新 `verification.md` / `changelog.md` / `PLAN-custom-components.md` 的 M5 行。

## 5. 复现命令与量法（照抄即可）

```powershell
# 构建 + 离线 + ABI
./build.ps1 ; ./tools/abi.ps1

# 完整门禁（约 90 秒）
./tests/text-box-playtest.ps1

# 保留选中状态运行，用于量"游戏白弧"（脚本会在缺少取消选中行时报错，属预期）
$env:TC_TEXTBOX_KEEP_SELECTION='1'; ./tests/text-box-playtest.ps1

# 对照：让游戏白弧保持开启
$env:TC_TEXTBOX_KEEP_SELECTION='1'; $env:TC_TEXTBOX_KEEP_ARCS='1'; ./tests/text-box-playtest.ps1

# 从日志里取"最后一次报告的 footprint 框"，内缩 18px（=0.65 格）后数白像素
./build/text-box-out/measure-arc.ps1
```

量法要点：**必须用日志里 `PASS text-box selection hint ... box=...` 报告的坐标**去内缩取窗口，
不要手算坐标——本会话就因为手算坐标落空得到过一次假的"0 像素"。内缩 18px 的理由：Mod 自己的白环画在
框边上（线宽 2.5px + 4% 内环），内缩后窗口里只剩游戏那圈弧。

第二轮起有两个窗口要量，`measure-arc.ps1` 量的永远是**日志里最后一行的那个实例**：抑制场景里
最后选中的是未被抑制的对照探针（应得数百），`TC_TEXTBOX_KEEP_ARCS=1` 灵敏度对照里最后选中的是便签
本身（应得数百，见 `docs/verification.md`）。脚本内已经按这两个数值写了断言，手量只是复核。

基准数字（同一便签、同一个 footprint 框、unit=28.16）：

| 场景 | footprint 内缩窗口白像素 |
|---|---|
| 游戏白弧开着（对照） | 541 / 543 |
| 游戏白弧按理应被关掉（当前实现，无效） | 542 / 543 |
| Mod 自绘橙色环的像素（诊断色，`TC_TEXTBOX_HINT_ORANGE=1`） | 1347，bbox `1176,236..1402,350` = 8×4 格 |

## 6. 测试开关与辅助脚本

| 名字 | 作用 |
|---|---|
| `TC_TEXTBOX_KEEP_SELECTION=1` | 拖动后不清选中，截图里保留选中提示（量弧线用） |
| `TC_TEXTBOX_KEEP_ARCS=1` | 不调用 `set_selection_hint`，作对照 |
| `TC_TEXTBOX_HINT_ORANGE=1` | 自绘提示改成橙色，便于与游戏白弧区分 |
| `TC_TEXTBOX_DEFAULT_PROBE=1` | 放置/default-drawing 对照组（由播放脚本设置） |
| `TC_TEXTBOX_DRAG_PROBE=0` | 跳过拖动阶段（分离"拖动残留"与"关掉绘制"） |
| `TC_TEXTBOX_ARC_ZOOM_OUT=<n>` | 弧线场景选中便签后再缩小 n 档（诊断用）。门禁会跟着这条一起验证"布局格子数不随缩放变"，`n=8` 时整套断言仍全绿 |
| `TC_TEXTBOX_LAYOUT_TRACE=1` | 每秒把便签布局以板面格打进日志（`box/button/text/padding cells`）；弧线场景由脚本自动打开，用来断言"整套外观随相机缩放" |
| `TC_SELECTION_TRACE=1` | 宿主选中集/精灵的 trace（2026-09-23 新增；旧 `TC_FOCUS_TRACE` 已随死靶删除） |
| `TC_TEXTBOX_BLOCK_BOARD_INPUT=0` | 关掉"节点独占鼠标"（2026-09-23 新增）：同一段代码、同一串合成事件下棋盘会重新拿到按下，导线在松开那一帧回来。灵敏度对照用，见 [verification.md](verification.md)「节点独占鼠标」 |
| `build/text-box-out/measure-arc.ps1` | 从日志取框 → 内缩 → 数白弧像素 |
| `build/text-box-out/hint-pixels.ps1` | 同一窗口数橙色/白色像素 |
| `build/text-box-out/crop.ps1` | 从进程内 BMP 裁图做视觉核对 |

相关证据图：`build/text-box-out/hint-selected-box.png`（Mod 白/橙环 + 游戏弧）、`hint-dragged.png`、
`hint-measured.png`、`zeropin-note.png`、`zeropin-hidden.png`。

## 7. 真机部署与回退（2026-09-23 第二轮已部署）

**2026-09-23 第二轮已部署**（门禁全绿、且沙箱里跑的正是同一份字节：部署后
`dist\tc-loader.dll` 与 `D:\p\game_engine.dll` 的 SHA256 相同，
`dist\local.text-box.mod` 与 `D:\p\mods\local.text-box.mod` 相同）。部署命令照旧：

```powershell
Copy-Item D:\p\tc-modloader\dist\tc-loader.dll D:\p\game_engine.dll -Force
Copy-Item D:\p\tc-modloader\dist\local.text-box.mod D:\p\mods\local.text-box.mod -Force
& D:\p\tc-modloader\dist\tcmod-cli.exe D:\p enable local.text-box
```

这一轮部署前的两份现场已按同样的命名规则备份：

| 备份 | 内容 |
|---|---|
| `D:\p\game_engine.dll.bak-20260923-before-selection-hint` | 9/23 12:19 的加载器（钩子还挂在死靶上） |
| `D:\p\mods\local.text-box.mod.bak-20260923-before-arc` | 上一版文本框包（还没有弧线范围对照） |

回退 = 把上面两份复制回原名，再跑一次 `tcmod-cli.exe D:\p enable local.text-box`；
更早的现场也都还在：

| 备份 | 内容 |
|---|---|
| `D:\p\game_engine.dll.bak-20260923-before-text-box-v2` | 9/22 的旧加载器（无 render V2/V3） |
| `D:\p\mods\local.text-box.mod.bak-20260923` | 旧文本框包（1 根悬空输出脚那版） |
| `D:\p\mods\local.text-box.mod.bak-20260923-onepin` | 1 脚 + V2 那版 |

回退后按 `tcmod-cli.exe D:\p enable local.text-box` 重新部署一次。

## 8. 同一轮里其它已经定型的事实（避免重复踩）

- **真 0 脚已成立**：`examples/text-box` 现在注册真 0 进 0 出（`shape=0in/0out`），实例照常绑定、
  照常出现在 `tc.board` 元件序列里；详见 `docs/verification.md`「文本框改回真 0 脚」。
- **footprint = 碰撞箱**，用户已明确表示保持现状，不要再做分离。
- **拖动用例的抖动已修**：编辑器刚关就发拖动按下会被吞掉（实测出现过 `-6,-18 -> -6,-18`），
  现在先 `endEditing()`、下一帧再按。
- `focus_components__...u5084` 在本构建里**不是**棋盘选中高亮的绘制者，不要再拿它做实验。
