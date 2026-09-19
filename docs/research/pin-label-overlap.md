# 引脚名重叠（底部元件栏预览）研究记录（2026-09-19）

> **本文 §1–§3 是调查过程，其中一条结论后来被推翻**：引脚名确实是 ImGui 文本
> （`build_custom_component_preview` 里的 `igText`，见 §5）。当时"文本钩子看不到引脚名"
> 是因为探测运行里屏幕上并没有那个预览。最终做法见 **§5.2**。

## 0. 问题

玩家的自定义元件如果引脚名很长，**底部元件栏**里的元件预览上，引脚名会互相压住
（本质：引脚名永远横向绘制，而上下排布的引脚是横向并排的）。目标：名字放不下时改成
斜排（45°，方案 B），短名字保持横排。

## 1. 已经查实的链路（都有地址）

```
底部元件栏（游戏自己的 bottom panel）
 ├─ build_custom_component_preview__presenterZboard95uiZbottom95panelZcommon_u486   RVA 0x38d990
 │    · 面板自己的标题/说明用 ImGui 画：igPushFont(2) + igCalcTextSize + igText
 │      （在 build_component_description_panel__..._u1227，RVA 0x39b400 里可以看到）
 │    · 元件本体 + 引脚名不是实时文字，而是**渲染好的快照贴图**
 └─ render_component_snapshots__main_u414                     RVA 0x47ffe0
      ├─ get_captured_path__presenterZio_u15/u25/u28          RVA 0x274160 / 0x274360 / 0x274500
      │    （快照文件路径，即 asset/capture/com_*.png）
      └─ update_state_snapshot__presenterZupdate95state95snapshot_u9   RVA 0x47e450
           └─ get_label_offsets__presenterZcomponent95snapshotZsnapshot_u127  RVA 0x38df80
                · 读原型（get_prototype__modelZboardZcustom95prototype95list_u502）后算出
                  每个引脚标签的位置；**没有任何宽度适配**
           └─ add__presenterZcomponent95snapshotZsnapshot_u150     RVA 0x3fda0
                · 把标签加入快照的标签表
```

渲染器还有专门的**快照模式**：`is_snapshot_mode__presenterZrendererZmulti95meshZmulti95mesh_u270`
（RVA 0x27c880）——这解释了为什么**电路板上只画元件名、不画引脚名，而快照/元件栏里会画引脚名**。

## 2. 为什么之前所有文本钩子都看不到引脚名

已逐个挂钩验证（都是只读探针，未改行为）：

| 钩子 | 结果 |
|---|---|
| `igRenderText`（引擎导出） | 看不到引脚名（该构建的 ImGui 文本不走它） |
| ImGui 内部 TextEx（`tc_game_engine.dll+0xfb2d0`，由 `igTextUnformatted` 包装跳转而来） | 能看到游戏自己所有 ImGui 文字（主菜单 play campaign 等），**但看不到引脚名** |
| `ImDrawList_AddText_Vec2`（引擎导出） | 看不到 |
| `label_mesh.set_text` / `fading_label_mesh.set_text`（EXE RVA 0x286600 / 0x2a4c10） | 只看到关卡 IO 名（长度 ≤9），**看不到长引脚名** |
| `to_markup_text`（EXE RVA 0x3690a0，markup 文本转换） | 在关卡提示模式下 0 次调用 |

原因：引脚名是**快照渲染**产物（见 §1 链路），既不是 ImGui 文本，也不是 UI 实时标签网格。

## 3. 死路（已验证，别再试）

- 挂钩 draw-list 文本的**内部**函数（`tc_game_engine.dll+0xe9100`）会让游戏卡在错误对话框；
  两种参数顺序都试过（`(self,font,...)` 与 `(font,self,...)`），并把“按长度匹配”而不是
  `strstr`（该函数收到的是“起止指针”，越界读会崩）——都不行。**不要再用这条路。**
- 右键不会打开元件菜单（游戏里右键 = 删除，见 translations 字符串）。
- 元件栏预览**不是**由“点元件/悬停元件”触发（都用光标驱动试过）；它出现在选中元件菜单里的
  元件时（玩家截图那种状态）。

## 4. 复现与验证工具（已就位）

| 工具 | 作用 |
|---|---|
| `tests/pin-label-playtest.ps1` | 沙箱 + 装 mod（可 `-ExtraMods`）+ 指定关卡/存档/板子 + 定时抓帧转 PNG |
| `tests/long-pin-fixture.cpp` | 声明式注册“字节加法器 + 超长引脚名”，自带 autotest（加载关卡） |
| `tests/cursor-pin-driver.cpp` | `dev.cursor-pin`：把真实光标钉在指定游戏坐标，可左键/右键点一下 |
| `TC_MODLOADER_TRACE_TEXT=<子串>` | 钩 ImGui 内部 TextEx（`*` = dump 前若干条） |
| `TC_MODLOADER_TRACE_LABEL=<子串>` | 钩两条标签网格 set_text + `to_markup_text`（`*` = dump） |
| `tools/call-sites.js` | 反查某地址的调用点并标出所在函数 |

已知可用的界面操作（用于自动“走进”那个画面）：

```
棋盘顶栏（game 坐标，2560x1452 画布）：46=菜单 110=Level objectives 176=Switch schematic
                                        240=Manual 345=Foundry(元件编辑) 540=提示灯泡
右侧竖排标签 BIT/WORD/MISC/IO/CUSTOM（x≈2515，y=150..515）= 元件菜单分类页签
```

## 5. 结论（已实现，2026-09-19）

引脚名既不是标签网格、也不是快照贴图，而是**元件预览自己用 ImGui 文本画的**：
`build_custom_component_preview`（EXE RVA 0x38e990）对每个引脚做一次
`igCalcTextSize` → `get_label_offset` → `igSetCursorPos` → `igText`，那次 `igText` 调用在
EXE RVA 0x38f0ea，所以被调函数看到的返回地址是 **0x38f0ef**。同一个函数既被底部元件栏
（`build_component_description_panel`，调用点 0x39dc09）使用，也被元件工坊
（`build_foundry_panel`，调用点 0x3aeede）使用——一处修改覆盖两处画面。

之前所有文本钩子“看不到引脚名”是因为探测时屏幕上根本没有那个预览（fixture 元件在棋盘上，
没有打开工坊预览），不是路径不同。

### 5.1 第一版（已删除）：改标签网格的旋转字节

先在**标签网格**这一层做了 45°（`hookSetText` + `TC_MODLOADER_PIN_ROTATE`），实测能转
（旋转字节每档 45°，返回地址 0x472f29 那次调用），但那只作用于**棋盘上的**元件/IO 名，
不是玩家看到的预览，属于改错地方，**已从 `src/loader.cpp` 删除**（同时删掉了 0xe9100 那条
会让游戏卡死的死钩子）。

### 5.1.1 中途试过、已放弃：把名字摊开/倾斜/缩小

第一版修好之后又发现两种情况都不行：名字放宽 470 px 而同一排引脚只隔 24 px，
一个 400 px 高的面板里无论怎么**摊开、倾斜（最多 45°）、缩小**，要么冲出面板、要么压到下一排
文字、要么把名字甩到离引脚很远的地方（实测截图 `build/pin-label-out/theirs10-preview.png`、
`theirs11-preview.png`、`theirs20-preview.png`）。逐字符旋转绘制的代码也写了，
最后全部删掉。

### 5.2 最终做法：预览只画编号，名字全在表格里

| 项 | 做法 / 实测 |
|---|---|
| 挂钩点 | 引擎导出 `igText`（`tc_game_engine.dll` RVA 0x21520），只接管返回地址 = 0x38f0ef 的那一次调用；其余 193 个调用点原样转发 |
| 覆盖哪些画面 | 底部元件栏／元件工坊的预览（返回地址 **0x38f0ef**，白条在 0x38efb1）：编号 + 表格；**元件工坊里"编辑外观"的编辑器**（`build_editor`：名字返回地址 **0x3ae126**，白条 0x3adf96）：**只编号、不画表格**——那一屏整面板都是游戏自己的调色板，玩家是来改颜色的，表格只会压住颜色（玩家反馈过）|
| 变参转发 | 钩子写成 `void(const char*,...)` + `va_start`，转发给同库的 `igTextV`（RVA 0x21590），所以任何格式化文本都不受影响 |
| 谁被编号 | **所有引脚**：预览里不再出现引脚名，名字只出现在表格里（玩家要求"标签名和编号分开，预览图显示的是编号"） |
| 编号怎么给 | 由加载器实时分配：先由名字反推出引脚位置（上下边引脚的名字以引脚为中心；左右边引脚的名字贴着元件画，引脚在朝向元件的那一端），再用这些引脚的外框按**顺时针**走一圈（上边从左到右 → 右边从上到下 → 下边从右到左 → 左边从下到上），从左上角开始编号递增 |
| 编号画在哪 | 每个编号**正对自己引脚**、紧贴该引脚那条白线的**外端**（间距 4 px），同一条边上的编号在**同一行**；朝外会顶到面板边时（例如位于面板底部的下边引脚）整条边改成画在**内侧**，避免被裁掉 |
| 编号多大 | 先按"每条边的跨度 ÷ 该边引脚数"量出引脚间距（不用最近邻：手绘元件可能有两个引脚几乎重合，一个这样的对会把整图字号带小），取最密的那条边，字号 `min(0.65, 0.85×间距 / 两位数字宽)`。**试过把编号错开成两排来换更大字号，结果是"看着错位、很乱"**（玩家反馈），所以固定一排，间距小的时候字号就小——对齐比字号重要 |
| 编号颜色 | 预览里的文字是暗的，编号只有一两位数字要读，所以用**纯白**画（`igPushStyleColor_Vec4(0)`）|
| 引脚白线段 | 预览给每个引脚画的那根白条（`build_custom_component_preview` 里的 `ImDrawList_AddRectFilled`，返回地址 0x38efb1）现在只用来标位置，所以加载器把它**厚度减半、长度缩到一半**；缩短时**固定贴近引脚的那一端**（早先按中心缩短，结果白条反而离引脚更远，玩家反馈"离得有点远"）。这根白条还是"引脚在哪、在哪条边、朝外是哪边"的唯一精确依据：它的矩形被记在本帧的引脚记录里，下一帧编号就按它来定位 |
| 表格画在哪 | **优先右侧**（右侧装得下就放右侧，否则才放左侧）：这些面板的左侧有游戏自己的控件（元件工坊外观编辑器的调色板、元件栏的按钮），而加载器看不到它们，右侧往往更空；距面板上沿至少 60 px 以避开面板自己的角按钮 |
| 表格排版 | 字号默认 70%（`TC_MODLOADER_PIN_TABLE` 可调），行高不够就**分成多列**（最多 6 列）继续排，仍放不下才降到 35%；这一版修掉了"表突然不见了"（26 行的表在 402 px 高的面板里原本被直接放弃） |
| 字号怎么还原 | ImGui 把窗口字号存在窗口对象上（它的 `SetWindowFontScale` 就是写 `[window+0x308]`，实测），加载器直接读回这个 float 再写回去；早先按字号比例反推会被 ImGui 的字号取整（整数像素）逐帧带偏（实测 29→28），已于 2026-09-19 修正 |
| 分帧边界 | 加载器导出的 `igEnd` 每帧自增一个计数器，预览名字表因此在"上一帧完整数据"上排版；同一帧里边画边判会让同一排的名字得到不同的邻居和不同的位置（实测过） |
| 切换模式时的闪烁 | 编号必须依据**完整一帧**的引脚数据，所以切换预览／外观编辑器的那一帧本来没有数据可用——早先的做法是"认不出就按原样画"，于是那一帧会闪出原版那团重叠的名字。现在改成：认不出上一帧的名字这一帧**什么都不画**（编号下一帧就出来），表格也再等一帧；切换只表现为"名字→编号"的干净替换，中间最多空一帧（16 ms）。位置匹配也放宽到 2 px，面板轻微滚动不会误判成切换 |
| 关掉的方式 | `TC_MODLOADER_PIN_TABLE=0` 关闭；其它数字 = 表格字号百分比（默认 70） |
| 实测证据 | 用户存档 + 元件工坊：改前 `build/pin-label-out/theirs9-preview.png`（一排名字糊在一起）、放弃方案 `theirs20-preview.png`（名字散开但很乱）、只给放不下的编号 `theirs26-preview.png`，最终"全部编号 + 顺时针 + 多列表格" `build/pin-label-out/theirs32-preview.png`（26 个引脚：上 1–9、右 10–13、下 14–22、左 23–26，左侧两列表格一一对应）；日志 `Preview pin table ... rows=26 scale=0.50 columns=2 width=927 -> drawn` 说明表为什么这样排 |
| 回归 | `tests/native-component-playtest.ps1`、`tests/native.ps1`、`tests/ui-board-panel-playtest.ps1`、`tests/wire-palette-tool-playtest.ps1` 全部通过 |

复现命令（用户存档 + 光标驱动点开工坊按钮）：

```powershell
$env:TC_CURSOR_CLICK='1'
.\tests\pin-label-playtest.ps1 -Name theirs17 -Seconds 140 -ShotDelay 56000 -CursorDelay 40000 `
  -Profile 'C:\Users\Administrator\AppData\Roaming\Turing Complete Mods\profiles\import-e93e633f1ae7701b50bc8576' `
  -ExtraMods 'dev.enter-board','dev.cursor-pin' -CursorPoint '345 37'
```

（`-CursorDelay` 要够晚：点击落在地图主页上时只会进关卡树，落进棋盘后才会开工坊。）

## 5.3 调查中留下的、仍可复用的下一步工具

1. 让元件栏进入“元件预览”状态：点右侧 `CUSTOM` 页签展开列表，再点其中的元件缩略图
   （`custom1.png` 里已能看到展开后的缩略图槽位）。状态对了以后：
   - `TC_MODLOADER_TRACE_LABEL='*'` 应能看到长度 ≥10 的标签（引脚名）；
   - 若仍看不到，说明标签确实不经 `label_mesh`，那就直接钩 `get_label_offsets`（RVA 0x38df80）
     做只读探针（注意它吃 Nim 对象，先用“只计数、原样转发”的方式，不要解析参数）。
2. 拿到引脚名标签的**变换**后，在那一层加判断：标签宽度 > 该引脚可用宽度 → 把旋转字节
   （标签网格的 transform `+0x10`，单位 1/256 圈，45° ≈ 32）改成斜排。
3. 验证：优先看**重新生成的快照 PNG**（`asset/capture/com_*.png`）——它把引脚名烤进图里，
   改前/改后各一张图就能判定，不需要对着元件栏截图。

## 6. 备注

- 本构建（0.4.0，ImGui 1.92.6）所有地址均为实测：EXE 符号可用 `nm "Turing Complete.exe"` +
  `tools/call-sites.js` 反查，`tc_game_engine.dll` 的导出 RVA 需要自己读导出表
  （`objdump -p` 第三列是序号，不是 RVA）。
- `TC_MODLOADER_TRACE_TEXT` / `TC_MODLOADER_TRACE_LABEL` 探针只在设置环境变量时才挂钩；
  引脚名排版是默认开启的功能，只接管预览那一次文本调用，正常游玩对其它文字没有影响。
- 相关更早的结论见 [native-ui-handoff.md](native-ui-handoff.md)、
  [toolbar-tool-handoff.md](toolbar-tool-handoff.md)（工具栏文字问题的调查过程与坑）。
