# 元件属性编辑器 M 阶段交接：`tc.component.editor` V1 第一刀

日期：2026-09-27
工作区：`D:\p\tc-modloader`
状态：**V1–V4 全部落地。** 离线门槛（`tests/component-editor.cpp`，含 V4 的取色字面量、位集与表级
开关）与真机门槛（注册、复选框行、文本行、单选行、十六进制行、长表滚动、下拉、拒绝、双击、右键菜单、
缩放抓手、颜色拖动、面板折叠、调色板、混合取色、校验标记、暂存 Apply/Cancel）全部通过；
最新一刀见 §8（2026-09-29）。

## 1. 为什么先做这一件

`docs/PLAN-custom-components.md` §14 要的是"声明式属性 schema"。在动手前先量了一下现状：

- 加载器已经把底部元件面板钩住，Mod 可以用 `TC_UI_SLOT_BOARD_COMPONENT_PANEL`
  把行画进面板；
- 但**那个窗口里的控件收不到输入**（实测 `IsWindowHovered` 对抽屉窗口为 0），所以
  `examples/float-ops` 只能自己再开一个无背景窗口盖在面板上、每帧不抢焦点、用
  `isItemDeactivatedAfterEdit` 提交——这套配方每个 Mod 都要抄一遍；
- 于是"属性编辑器"的真实缺口不是"没有绘制能力"，而是**没有宿主拥有的、能收输入的声明式行**。

第一刀就把这套配方收进宿主：Mod 交一张字段表，宿主画行、收鼠标键盘、按 `storage` 的
同一条路提交。

## 2. 这一刀交付了什么

| 层 | 文件 | 内容 |
|---|---|---|
| ABI | `sdk/tc_service_api.h` | `TC_SERVICE_COMPONENT_EDITOR`、`TCComponentFieldV1`、`TCComponentEditorApiV1`、字段种类/标志/错误码/上限 |
| SDK | `sdk/tc_component_editor.h` | `table()` / `setFields()` / `clearFields()` / `errorText()` |
| 模型 | `src/component_editor.hpp` | 字段复制与校验、小端配置读写、钳位、枚举轮转、显示文本（纯逻辑，无 ImGui/无游戏状态） |
| 宿主 | `src/native.hpp` | 服务协商与所有权检查、按类型保存字段表、在抽屉里画行、`igInput*` 控件、提交走 `storageWriteConfig`、`TC_EDITOR_TRACE` 行矩形日志 |
| 绑定 | `src/native_logic.hpp` | `instanceHandleOf(customId, instance)`：把抽屉给的 (类型, 实例) 对换成带 generation 的句柄 |
| 示例 | `examples/editor-demo/` | 0 引脚、32 字节配置、5 个字段（bool/uint/enum/text/float）的元件，只交表不写界面 |
| 离线 | `tests/component-editor.cpp` | ABI 静态断言、五种字段的校验与拒绝、小端往返、钳位、枚举轮转、文本截断、NaN 位型 |
| 真机 | `tests/component-editor-playtest.ps1` + `tests/component-editor-driver.cpp` | 注册门槛与行驱动门槛（点复选框、点文本行并打字 + Enter），断言落到元件 Mod 自己的 `on_config_changed` |

## 3. 语义（已冻结的部分）

- 声明时机：只在游戏线程的 `tc_mod_load` 里，且只能声明**调用方自己的** `custom_id`；
- 字段只能落在该类型注册时声明的 `config_size` 之内；越界、未知种类、宽度不对、枚举没有
  选项、UINT 超过 31 位、文本不足 2 字节或超过 64 字节，一律整表拒绝并写日志说明原因；
- 行由宿主绘制在游戏自己的元件抽屉上方（无标题栏/无背景/不抢焦点，位置与
  `examples/float-ops` 的可用配方一致）；
- 提交走 `storageWriteConfig`，因此**一次提交 = 一步撤销**，并触发该类型的
  `on_config_changed`；
- 编辑期间宿主保留该实例配置的工作副本；提交成功后下一帧按新的 `config_revision` 重新读取；
- `TC_COMPONENT_FIELD_HEX` 的 UINT 字段在 V1 只读显示（V1 整数控件是有符号十进制框）；
  `TC_COMPONENT_FIELD_READ_ONLY` 的字段只画不编辑；
- 不声明字段的类型行为完全不变；Mod 仍可用 `TC_UI_SLOT_BOARD_COMPONENT_PANEL` 画自己的
  编辑器，宿主行画在插槽之后。

## 4. 证据

```powershell
./build.ps1                      # 含 tests/component-editor.cpp（离线模型）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\abi.ps1   # 基线 1281 条
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1                 # 注册
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows      # 复选框行
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row label -Keys xy   # 文本行
```

真机（2026-09-27，隔离沙箱，pinned build）：

```text
Native logic: registered custom 0x454449545f303031 inputs=0 outputs=0 shape=0in/0out v2 state=0
Component editor: type 0x454449545f303031 edits enabled(bool), period(uint), mode(enum), label(text), scale(float)
[local.editor-demo] editor-demo: declared 5 editor fields for type 0x454449545f303031
Native loaded: local.editor-demo; hooks=0; declared capabilities: log, services
PASS component editor registration: the type and its field table are accepted by the pinned build
```

## 5. 还没验的（下一刀，必须先做）

**行驱动已经跑通**（2026-09-27）。做法：`tests/component-editor-driver.cpp` 用一个 Mod 干完玩家
会干的事——挂 `handle_update_wire` 拿到 model → 自己 `load_level("sandbox")` → 等场景稳定 →
用**命令总线**放一个 `EDIT_001` 实例 → 用棋盘自己的 `select_component` + 真实点击元件本体打开
抽屉 → 从 `loader.log` 读宿主自己打出的行矩形（`TC_EDITOR_TRACE=1`）→ 用真实鼠标点那一行 →
（文本行再打字 + Enter）→ 把结论写进 `result.txt`。脚本
`tests/component-editor-playtest.ps1 -Stage rows`（默认点 `enabled` 复选框；
`-Row label -Keys xy` 走文本输入）断言三件事：宿主画出了那一行、宿主提交了
（`Component editor: committed field …`，且没有被拒绝的写入）、以及**元件 Mod 自己的
`on_config_changed` 打印出了新值**——默认 `enabled=1`，点一下之后必须是 `enabled=0`；
文本行是 `label="…xy…"`。

真机证据（隔离沙箱，pinned build）：

```text
DRIVER: model=0x3c541e2260 board handle -> 0x3c541e2260 same=yes
DRIVER: the game's own placement helper returned true at 46,6
DRIVER: asked for one editor-demo component at 30,0 (request 1)
DRIVER: the placement command succeeded
DRIVER: the component is on the board as instance 0x3979605f587d8649
DRIVER: clicked the component body at 2047,800 (attempt 1)
Component editor: row instance=0x3979605f587d8649 field="enabled" rect=972,1409,1023,1460 active=0 focused=0
DRIVER: clicked the enabled row at 997,1434
[local.editor-demo] editor-demo: instance 0x3979605f587d8649 config changed enabled=0 period=16 mode=RNE label="hello" scale=1
Component editor: committed field "enabled" on instance 0x3979605f587d8649 value=off
PASS component editor rows
```

文本行（`-Row label -Keys xy`）多出来的两步证据：

```text
DRIVER: clicked the label row at 1407,1503
Component editor: row instance=0x6458… field="label" rect=972,1478,1842,1529 active=1 focused=1 buffer="hello"
DRIVER: typed "xy" into the label row and pressed Enter
[local.editor-demo] editor-demo: instance 0x6458… config changed enabled=1 period=16 mode=RNE label="helloxy" scale=1
Component editor: committed field "label" on instance 0x6458… value=helloxy
```

### 第一次尝试踩到的三个坑（抄这个用例时注意）

1. **`tcmod-cli apply` 是"设定启用集合"，不是"追加"。** 脚本里先 `apply local.editor-demo` 再
   `apply dev.component-editor-driver`，第二次把第一个顶掉了：`state.json` 里只剩驱动，元件类型
   从来没注册过，于是命令总线放元件只能返回 `-6`。现在两个 id 在**同一次** `apply` 里，并且脚本
   会从 `state.json` 断言两个包都真的启用。这条也解释了当时"游戏提前退出"的假象。
2. **`PLACE_COMPONENT` 在目标格已被占用时返回 `FAILED/STATE`（-6）。** 驱动自己的对照放置占了
   (30,0) 之后，命令总线再放同一格就被拒；现在对照放在 (46,6)，命令总线从 (30,0) 开始，被拒时
   依次换下一个空格。
3. **行窗口必须留在屏幕内。** 抽屉在屏幕底部，五个字段的行窗口按原来"抽屉高度 52%"起画时，
   最后一行落在 y=1601..1652（客户端只有 1600 高）——画得出来但点不到。宿主的行窗口现在用上一帧
   自己的高度把它夹进屏幕（`active=1 focused=1` 就是夹紧之后才出现的）。

## 6. 下一步之后（同一服务的后续版本）

## 6. V2（2026-09-27，同一天第二刀，已完成）

V1 之后的第二刀把 `tc.component.editor` 提到 **V2**，内容就是前面列的"少量布局标志 + 滚动 +
枚举单选 + 可编辑十六进制"：

| 新增 | 说明 |
|---|---|
| `TCComponentEditorApiV2` | 与 V1 同样的两个调用；**能查到 V2 才说明加载器认这些 flag**，V1 加载器会忽略它们 |
| `TC_COMPONENT_FIELD_FULL_ROW` / `INLINE` / `SEPARATOR` | 整行控件、与上一字段共用一行、本行前一条分隔线 |
| `TC_COMPONENT_FIELD_RADIO` | ENUM 铺成一行单选（float-ops 今天手画的形状） |
| `TC_COMPONENT_FIELD_HEX` | UINT 变成可编辑的 `0x…` 文本，宽度上限 31 位 → **64 位**；解析失败拒绝并保留旧字节 |
| 表级校验 | 首行不能 INLINE；`FULL_ROW`/`RADIO` 之后不能 INLINE；最多两列；未知 flag 位拒绝 |
| 滚动 | 行窗口按**内容**高度夹进屏幕（高度 + 滚动范围），再用尺寸约束封顶在屏幕内，剩下的交给 ImGui 滚动 |
| 诊断 | 行行 `window instance=… pos=… size=… rows=… scrollmax=…`；画不出行时给出 `skipped (原因)` |

示例 `examples/editor-demo` 现在有两个类型：`EDIT_001`（7 个字段，覆盖 bool/uint/enum-radio/
float/hex/text，含分隔线与一行两个字段）和 `EDIT_002`（32 行，用来压出滚动）。

真机证据（六条用例，全部进 `tests/test-catalog.json`）：

```text
component-editor        注册 + 字段表（含 V2 flag）被接受
component-editor-rows   点复选框 → committed → on_config_changed enabled=0
component-editor-text   点文本行（active=1 focused=1）→ 打字 → 回车 → label="xy"
component-editor-radio  点单选行最后一个选项 → mode=RNA
component-editor-hex    点十六进制框 → 打字 → 回车 → mask=0x…beef
component-editor-long   32 行：window … rows=32 scrollmax=458，第一行照样点击并提交（f0=0 ones=31）
Result: 6 passed, 0 failed
```

### V2 这一刀新踩到的四个坑

1. **夹紧要用内容高度。** 先按"窗口高度"夹紧、再封顶，会把 7 行的编辑器压成屏幕上沿那两三行并
   开始滚动。现在的顺序是：内容高度（上一帧窗口高度 + 滚动范围）→ 夹紧 → 尺寸约束封顶。
2. **驱动的点击会抖。** 同一份代码、同一个坐标，有的运行点得开抽屉、有的点不开（点了 8 次都不
   行）。驱动现在每次重试都在目标点周围小范围补点（正中心 + 一圈 ±20/±40），最多 40 次——这是
   玩家会做的事，也让"行驱动"这类用例稳定下来。
3. **键盘的编辑键到不了游戏里的输入框。** Home/Delete/Backspace/Ctrl+A 都不生效（`Ctrl+A` 甚至
   连修饰键状态都带不过去），只有**字符**能进 ImGui 的 `InputText`。所以十六进制用例断言的是
   提交值**包含**输入的数字，而不是等于它。
4. **用例的说明文本必须保持 ASCII。** `…` 这类字符会让 PowerShell 5.1 按 GBK 读 `test-catalog.json`
   时解析失败（见 [HANDOFF-next-steps.md](HANDOFF-next-steps.md) 的"干活的规矩"第 5 条）。

## 7. V3 已完成（2026-09-28）：下拉、颜色、自定义行、校验、事务与面板约束

上一节列的"还没做"全部落地，外加玩家当天追加的四条界面要求。**先读这一段，下面的 V1/V2 描述只作为
历史背景。**

### 交付物

| 能力 | 落点 |
|---|---|
| 枚举下拉 | `TC_COMPONENT_FIELD_DROPDOWN`；宿主在 `igBeginCombo` 分支里画并提交 |
| RGBA8 颜色 | `TC_COMPONENT_FIELD_COLOR_RGBA8` = 8；`igColorEdit4`，拖动一步撤销 |
| 自定义行 | `TC_COMPONENT_FIELD_CUSTOM` = 9 + `TCComponentEditorCustomRowV1` + `TCComponentRenderDrawV1` |
| 右键菜单 / 双击 | `TCComponentEditorActionCallbackV1`（`kind` 1/2），菜单每行最多 8 条且 id 非零不重复 |
| 缩放抓手 | `TC_COMPONENT_FIELD_RESIZE_HANDLE`：按下开 `storage` 事务、拖动发累计位移、松手提交（一步撤销） |
| 行内校验 | `TCComponentEditorValidateV1`：`OK`/`WARNING`/`ERROR`，错误拒绝并保留旧字节 |
| 编辑事务 | `TCComponentEditorApiV3::begin_edit/commit_edit/abort_edit`（转发 `tc.component.storage` V2） |
| 面板约束 | 行窗口严格夹在游戏底部面板内；每帧重算；装不下时滚动 |
| 折叠行为 | 折叠时不画也不动状态；拉起后同矩形、同 buffer |
| 说明文字 | V2 定义默认不把 `description` 写进原型；`TC_COMPONENT_TYPE_SHOW_DESCRIPTION` 选开 |

### 这一轮踩到的四个坑（都用真机日志定位，抄用例时别再踩）

1. **combo 弹窗会移动**：`igBeginCombo` 的弹窗第一帧出现在指针处，第二帧才翻到字段上方（底部抽屉
   没有向下空间）。驱动按第一帧的坐标点，点到的其实是行的其它部分。修法：宿主把弹窗与每个选项的
   矩形写进日志（`Component editor: popup …` / `popup item …`，打开时与变化时各写一次），驱动等矩形
   连续两帧不变再点。
2. **重叠 item 抢 ActiveId**：自定义行的画布 `InvisibleButton` 先提交，把 ActiveId 拿走，后提交的
   缩放抓手永远收不到点击（按下时宿主日志里连一行都没有）。修法：画布提交前调
   `igSetNextItemAllowOverlap()`。
3. **抓手会被窗口裁掉**：行是最后一行时它的右下角正好在窗口内容区之外（窗口底边比行底边高 1 px，
   内边距又在里面），抓手点不到。修法：抓手 14 px 起，并把它的上边夹到 `窗口底边 - 10 px`；同时
   把抓手自己的矩形也写进日志，驱动点它的中心而不是猜行的角落。
4. **折叠时钩子照样跑**：抽屉标题行的钩子在折叠状态下也会被调用，此时"标题行以下"只剩 1 px。判折叠
   只需要这一个数（< 34 px 视为折叠），并且要在**取配置、建编辑状态之前**返回，否则正在输入的文本
   会被重新读回配置覆盖。

### 仍未做

- 与 `tc.component.storage` 的 schema 迁移联动：字段表不参与迁移（迁移只改字节），偏移不变，
  这一点已经写死在契约里；
- 取色形状的第三层（取色盘弹窗、吸管、按通道分别编辑）——V4 给的是"色块条 + 十六进制框"，
  真需要一个完整取色器时再按同一形状加位。

（上一节列的三件——调色板 / 混合取色、校验图标化、确认 / 取消对话框——已在 §8 交付。）

### 证据与复现

```powershell
./build.ps1                                       # 含 tests/component-editor.cpp（离线模型，V3）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\abi.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1                     # 注册（含说明文字两种结果）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row mode -Gesture dropdown -Choice 1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row mode -Gesture dropdown -Choice 4
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row preview -Gesture grip
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row color -Gesture wiggle
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row label -Keys xy -Fold
```

用例 ID 与描述见 `tests/test-catalog.json`（`component-editor*`，V3 收尾时 13 条，§8 之后共 18 条）；
每条都在 `-Tier game` 下。

## 8. V4 已完成（2026-09-29）：取色另外两种形状、校验标记、确认 / 取消式暂存表

V3 留下的三件在这一刀补齐，做法都是**加法**：字段描述符还是 `TCComponentFieldV2`，V3 的表一字未改，
新形状挂在新的位和一个新的表级调用上，只有显式查 V4 的 Mod 才碰得到。

### 交付物

| 能力 | 落点 |
|---|---|
| 色块条 | `TC_COMPONENT_FIELD_COLOR_PALETTE`（位 8）；色块用 `choices`/`choice_labels` 声明，每个标签是一条颜色字面量，最多 16 个。宿主逐块发布矩形（`Component editor: swatch …`），点一块提交一块 |
| 混合取色行 | `TC_COMPONENT_FIELD_COLOR_HEX`（位 9）：同一行再多一个 `0xAABBGGRR` 文本框，也接受 `#RRGGBB[AA]`；读不懂就拒绝并退回实例的值。与 PALETTE 合起来就是"色块 + 精确字" |
| 一行式校验标记 | `TC_COMPONENT_FIELD_VALIDATION_ICON`（位 10）：错误红 / 警告琥珀的小方块画在控件末尾，消息进 tooltip（`validation tip …`）与日志。用窗口自己的绘制表画，不占 ImGui 项，所以不挤动下一行 |
| 确认 / 取消 | `TC_COMPONENT_EDITOR_TABLE_CONFIRM`（表级位 0）+ `set_fields_v3()`：整张表改成暂存，行下多出 Apply / Cancel。Apply 一次写完（一步撤销、一次 `on_config_changed`），Cancel 丢暂存 |
| 版本纪律 | `TC_COMPONENT_EDITOR_API_VERSION_4` + `TCComponentEditorApiV4`（V3 成员偏移不变，新入口追加在末尾）；`TC_COMPONENT_FIELD_V3_KNOWN_FLAGS` 把 V3 的位集固定下来，V3 入口按它校验 |

### 这一轮踩到的坑（抄用例时注意）

1. **宏别名会跟着重定义走。** `TC_COMPONENT_FIELD_V3_KNOWN_FLAGS` 最初写成
   `#define … TC_COMPONENT_FIELD_KNOWN_FLAGS`，而后者在 V4 段又被 `#undef`/重定义了一次——于是
   V3 的位集在**使用点**展开成 V4 的位集，V3 入口悄悄接受了 V4 位。离线用例
   `assert(!validate(…,TC_COMPONENT_FIELD_V3_KNOWN_FLAGS))` 当场抓住，改成逐个位写出来。
   凡是要"冻结一版"的常量都不能拿一个会改名的宏当代号。
2. **暂存表和 CUSTOM 行不能共存。** CUSTOM 行的回调自己经 `tc.component.storage` 写配置，宿主的暂存
   副本会与之抢同一个 blob（Apply 时会用陈旧的副本覆盖 Mod 的写入）。契约改成整表拒绝，错误信息写
   明是哪一条行导致。
3. **拖动取色的 storage 事务在暂存表里要跳过。** 非暂存表的颜色控件靠
   `storage begin_edit`/`commit_edit` 把一次拖动合成一步；暂存表根本不写，开了事务反而会留下一个
   永不提交的编辑。代码按 `staged` 分支跳过，整批只由 Apply 写一次。
4. **校验标记不能用 ImGui 项画。** 用 `invisibleButton` 之类提交一个项，光标就停在标记上，下一行的
   标签会横着排在标记旁边而不是掉到下一行。改用窗口自己的绘制表 + 手动命中（读 `igGetMousePos`），
   行高与排版一个像素都不动。

### 证据与复现

```powershell
./build.ps1                                       # 含 tests/component-editor.cpp（离线模型，V1-V4）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\abi.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1                     # 注册（含 V4 表）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row palette -Gesture swatch -Choice 2
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row palette -Keys beef
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Row mode -Gesture dropdown -Choice 4 -Hover
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Type staged -Dialog apply
powershell -NoProfile -ExecutionPolicy Bypass -File tests\component-editor-playtest.ps1 -Stage rows -Type staged -Dialog cancel
```

用例 ID 与描述见 `tests/test-catalog.json`（`component-editor*`，18 条）；每条都在 `-Tier game` 下。
