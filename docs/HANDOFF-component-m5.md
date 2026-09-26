# 元件接口 M5 交接：geometry 与 render V1 覆盖层已落地

> **选中提示（白弧）这条线单独交接**：[HANDOFF-selection-hint.md](HANDOFF-selection-hint.md)。
> 一句话现状（2026-09-23 已收口）：Mod 自绘的 footprint 提示环已完成并验证；**游戏自己那圈白弧也已按
> 类型关掉**——宿主钩子改挂 `redraw_selection__presenterZupdate95state95common_u5104`（`focus_components`
> 是死靶，其钩子已删除），在绘制帧里把该元件在选中集里的桶占位字清 0 再还原。真机逐像素：被抑制
> 窗口 0（截图帧仍选中该实例）、`TC_TEXTBOX_KEEP_ARCS=1` 灵敏度对照同一窗口 546。
>
> **V4 追加（2026-09-23 第五轮）**：`set_foundry_button(id,false)` 按类型去掉选中时底部面板右上角
> 那个"在元件工坊编辑"按钮（Mod 元件没有玩家可编辑的内部电路）。宿主只在那一次 `igButton` 调用
> （`build_component_description_panel` RVA `0x3a45ac`）上用全透明样式画并返回"未点击"，
> 面板布局与其它控件不变；真机同一 42×42 px 矩形：关掉 0 像素、`TC_TEXTBOX_FOUNDRY_BUTTON=1`
> 保留对照 654 像素。ABI 基线 1192 → 1203 条，新增记录全属 `TCComponentRenderApiV4`。
>
> **2026-09-23 第六轮追加（先读这一段）：render V2「关闭游戏默认绘制」已落地、已真机逐像素验收、
> 已进 SDK / ABI / 文档。** 下方所有"下一步做默认绘制关闭"之类的文字都属于历史，以本段为准。
>
> **交付的接口**：`tc.component.render` **V2**（完整包含 V1 前缀，只多一个字段）
> `int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);`
> 关掉的是游戏给自定义元件画的设计图缩略图、名字水印和相关默认网格；按类型生效，只能关调用 Mod
> 自己登记的类型。落地文件：`sdk/tc_service_api.h`（V2 结构与语义注释）、`sdk/tc_component_render.h`
> （`tableV2` / `setDefaultDrawing`）、`src/native.hpp`（`armDefaultDrawingControl()` 钩住
> `redraw_component__presenterZupdate95state95common_u4925` + `defaultDrawingHidden()`）、
> `tests/component-render.cpp`、`tools/abi-snapshot.cpp`、`abi/windows-x64.json`。
>
> **真机证据**（隔离沙箱，`tests/text-box-playtest.ps1`，全部是脚本里的硬断言）：
>
> ```text
> Component render: default drawing control armed
> text-box: game default drawing disabled for note type
> Component render: suppressed game default drawing custom=6072356791976472625 instance=<非零 id>
> PASS text-box default drawing probe centers hidden=1092.1,462.1 visible=1767.9,462.1
> PASS text-box default drawing pixels hidden=0 visible=350 want>=181 unit=28.16
> PASS text-box hidden note drag from -6,-18 to -5,-18 selection=1 instance=2480352624375808058
> PASS text-box hidden note deselected selection=0 cleared=1
> PASS text-box render transforms rotations=0xf unit=25.60/25.60 clip=1000x650
> PASS text-box render zoom live unit 25.60 -> 28.16
> PASS text-box render pan delta=150.00,0.00 axes unchanged instances=4
> PASS text-box render clip pixels left=800..831 right=1768..1799 top=200..231 bottom=818..849
> ```
>
> 复现命令（按顺序）：`./build.ps1` → `./tools/abi.ps1` → `./tests/text-box-playtest.ps1`。
> 证据图：`build/text-box-out/hidden-probe.png`（关掉绘制的实例只剩引脚点）、
> `build/text-box-out/visible-probe.png`（开着绘制的对照：青绿缩略图 + 水印）、
> `build/text-box-out/hidden-drag.png`（被拖动实例上的游戏选中提示环）。逐条说明见
> `docs/verification.md` 的「M5 render V2 关闭游戏默认绘制」一节。
>
> **两条容易踩的事实**：
> 1. 元件栏/元件选择器用 **id 为 0 的临时记录**预览，接管判据要求"带非零实例 id"，所以关掉图案
>    的元件在菜单里仍然有图案。上一轮没有这条判据时，真机日志里出现过 `instance=0` 的抑制行。
> 2. 拖动之后游戏会在**被拖动的那一个实例**上画自己的选中提示 UI（引脚外侧的白色断环 + 引脚标签）。
>    它属于游戏交互提示层，不是 V2 接管的默认绘制；`clear_selections__modelZboardZboard_u8323` 把选中
>    计数清回 0 之后，提示环在截到的帧里仍然在。所以像素测量必须用**没被拖动过的兄弟实例**，否则
>    提示环的抗锯齿像素会被误算成"默认绘制残留"。
> 3. 编辑器那一次点击原先在同一帧里连发 down/up，游戏会在采样前把对折叠掉，实测约每六次失败一次
>    （`the click did NOT open the editor`）。本轮把按下与抬起拆成两帧（`clickAtScreenPoint` /
>    `releaseClickAtScreenPoint`，与相机拖动同一形式），改后连续两次整用例通过。
>
> **本轮改的文件**：`sdk/tc_service_api.h`、`sdk/tc_component_render.h`、`src/native.hpp`、
> `examples/text-box/plugin.cpp`、`examples/text-box/mod.json`（补 `symbol` 能力声明）、
> `tests/component-render.cpp`、`tests/text-box-playtest.ps1`、`tools/abi-snapshot.cpp`、
> `abi/windows-x64.json`（基线 1173 → 1182 条，新增记录全部属于 `TCComponentRenderApiV2`，V1 记录
> 一条未变）、`docs/verification.md`、`docs/changelog.md`、`docs/PLAN-custom-components.md`、
> `docs/sdk/services.md`、`docs/sdk/README.md`、`docs/reference/limits.md`、本文档。
>
> **M5 还剩什么**（顺序建议）：①字体/纹理资源生命周期与设备重建——`text-box` 的正文（字号、斜体、
> 中文换行）现在仍由插件自己画，需要先有资源句柄切片；②正文完全迁移到宿主服务；③缺 Mod 可见占位
> （§10.1，证据基础是已进目录的 `component-placeholder` 用例：它的场景 3 直接回答"Mod 被停用后元件
> 还活不活"，接着要决定占位是宿主画的还是留给游戏）；④元件栏预览、放置预览与自定义 LED/七段显示器的
> 阶段 5 像素回读；⑤`tc.component.input` 的整框拖动与框选冲突验收（阶段 6）。
>
> **测试开关**：`TC_TEXTBOX_DEFAULT_PROBE=1` 才放置两个对照实例并跑默认绘制/拖动阶段；
> `TC_TEXTBOX_FOUNDRY_BUTTON=1` 保留游戏那个工坊编辑按钮（V4 的对照运行），
> `TC_FOUNDRY_TRACE=1` 让宿主每秒把该按钮的矩形与"是否隐藏"写进日志（门禁在弧线场景自动打开）；
> `TC_TEXTBOX_LAYOUT_TRACE=1` 每秒把便签布局以板面格写进日志；
> `TC_TEXTBOX_DRAG_PROBE=0` 跳过拖动阶段（这是本轮用来分离"拖动残留"与"关掉绘制"的对照实验，
> 跳过时隐藏窗口的宽口径像素计数是 0）。两者都只影响 `examples/text-box` 这个探针 Mod。

> 2026-09-23 最新追加：`tc.component.render` V1 最小覆盖层已实现（逐类型回调、宿主实例枚举、
> 四向旋转仿射基、裁剪、受控基础图元与 4096 条配额），文本框已声明 8×4 footprint 并登记 render
> 回调。离线构建与 ABI 已通过；rotation 0/1/2/3 的仿射轴与有效裁剪框已在真机自动断言通过。
> 相机缩放（25.60 → 28.16 px/格）与平移也已真机自动断言；平移门禁确认四个旋转实例同移
> 150 px、仿射轴不变，平移后的编辑按钮仍可命中。四边裁剪也已由真机 framebuffer 逐像素验收：
> 跨越 `800,200..1800,850` 的四色标记只保留框内半边。下一步做默认绘制关闭与资源生命周期。
> 本文下方“render 尚未开始”等旧交接文字仅保留历史背景，以本追加为准。

日期：2026-09-22（当日第二次交接，含对上一轮结论的更正）  
工作区：`D:\p\tc-modloader`  
状态：**M5 进行中，未完成。** 已交付 `tc.component.geometry` V1 与 `tc.component.render` V1
覆盖层。footprint 的放置、占位和整框拖动已有真机证据；文本框已声明 8×4 footprint，并在隔离
真机里实际进入 render 回调、完成编辑器自测与截图。仍待实现：关闭游戏默认绘制、字体/纹理资源
生命周期与设备重建、正文完全迁移、缺 Mod 可见占位。

## 2026-09-22 追加（第三轮）：放置路径的登记 bug 已修，命中读数恢复可用

先读这三条，它们推翻了本文件下面几节里关于"合成装置不可信"的结论：

1. **用户实测发现的根因**：命令总线放上去的元件**不在棋盘的命中状态里**——点不到、拖不动；
   用户用手从元件菜单放一个元件之后，先前放的那些**一起**变得可拖。对照反汇编，游戏自己的放置
   在 `add_component` 成功后还会调用 `upgrade(presenterSlot, 0x30)`，而放置命令漏了这一步。
   **已修**：新增别名 `board.after_place` + `armBoardRegistration()` 从游戏自身的调用里学习该指针，
   放置成功后重放同一步。验收：修复前按内置 AND 门本体 `key=MAX`（只有引脚格会中），修复后
   `selected=1 key=1 snapshot=1`，按空板回到 `key=MAX`。
2. **命中跟随"画出来的图形"**，而 `set_footprint(6,3)` 不改变图形：进程内截图里，12×6 的实例与
   默认 1×1 的实例画得**像素级相同**（都只是一个小标签框 + 引脚点）。所以
   `set_hit_box` V2 不能只是"声明更大的矩形"。
3. **上一版探针的两个读数问题已定位**：① "选中集永远为 1"的闩锁是**未登记状态**的产物，
   登记正常后按空板会取消选中；② `TC_HITBOX_MEASURE=move` 的 `posread=0` 说明位置读取一直失败
   （元件句柄在棋盘序列重建后失效），要改用"每次重新抓对象快照、按实例 id 取值"。
   `docs/research/component-hitbox-path.md` 第四、五节是完整证据链。

下一步顺序：① 用修好的登记 + 选中读数重跑命中图（已按 9×7 网格在跑）；② 若两边的命中区确实
相同，转向"让游戏画出覆盖整框的图形"这条 `set_hit_box` 路线；③ 再回到 render 与文本框迁移。

## 2026-09-23 追加（第四轮）：命中问题闭环——可拖区就是 footprint 框

**真人手测确认（用户对着探针实时画出的声明框操作）：可拖区域 = 声明的 footprint 矩形，框内任意
位置都能拖，包括游戏什么都没画的地方。** 两种形状都验过：12×6 带引脚的，和 12×6 **完全无引脚**
（0 入 0 出，即文本框那种）。它们画出来的图形都只有小标签 + 引脚点。

由此：

1. **"整框可拖"不需要 `set_hit_box` 新几何**——声明与盒子等大的 `set_footprint` 就是答案，而这
   已经在 V1 里。footprint 同时是占位/碰撞框，放大即真的占那么大地方（便签本来就要）。
2. 文档里所有"footprint 不改变命中"的说法作废；干扰源是三个：命令总线放置未登记（已修）、
   "选中集"闩锁、以及自动化交替拖动与邻居碰撞箱相互影响。
3. 自动化移动差分网格降级为辅助工具；**手感结论优先**。

下一步（M5 剩余）：① 把 `examples/text-box` 迁到"声明大 footprint"这条路上做整框拖动验收；
② 继续 `tc.component.render` 最小切片（宿主枚举实例 + 仿射基 + ImGui draw-list 原语）；
③ 资源生命周期与缺 Mod 可见占位。

## 2026-09-23 追加（第五轮）：原版元件外观的外形研究（阶段 5 的前置调研）

结论与产物见 `docs/research/component-appearance.md` §0（0.1–0.5）。要点：

1. **原版元件 = 一种类一张 PNG 精灵掩码**，名字由 kind 索引（`tools/component-sprites.js` 还原出
   125 条并与 125 个内置原型对上）；R/G/B 通道分别被 `component.frag` 换成该实例 group 的
   主体/描边/文字色（BIT 蓝、WORD 绿、IMMUTABLE 红、UNLOCKED 黄，文字白）。
2. **锚点与缩放**：画布以元件记录点为中心，**1 板面格 = 20 精灵像素**（四边形 = 画布/20：
   140x100 → 7x5、180x100 → 9x5、300x180 → 15x9）。全量 106 个 kind 扫描、101 个有读数，中位数
   0.0496/0.0495；长条/多引脚元件按轴拉伸，已列表。
3. 自定义元件（0x4e）没有精灵：设计图缩略图（32×32、4 bit/格、内部电路自动缩放）+ 名字水印；
   空定义完全不画——但**可点区域来自 footprint**，所以"隐形但整框可拖"是可行的（拼 §0 与命中结论）。
4. 引脚点/标签是**另一层**：BIT 输入 `#E5787D`、输出 `#E53D5C`（WORD 绿色一族），标签是深底白字
   小板画在主体外；位置与 kind 表引脚偏移一致。
5. 未完成：**四边形直接测量**（`TC_HITBOX_HIGHLIGHT=1` 想用选中高亮，本构建沙箱里看不到可见高亮；
   下一步读 mesh 顶点或用重叠/剪贴板半透明态）、引脚标签板的尺寸与避让规则、`com_imm_*` 触发条件、
   `component_addon` 排布、以及离群 kind 的复核（NOT/Register 的读数受同色状态叠画影响）。

## 先读结论

1. 当前可安全写入的 Prototype 局部矩形序列确实参与放置/占位，也能从实时实例按旋转回读。
2. **上一轮第 2 条结论转为"未证实"，不是"被推翻"。** 那次两点对照（`(1,0)` 不选中、`(2,0)` 选中，
   两种 footprint 相同）在修好装置后**仍然可复现**（`clean=1`、0 脏样本）；但它的装置有两个缺陷：
   ① 用 `SetCursorPos` 驱动真实光标、没隔离真实输入（离元件中心 40 板面格的空点因此读成过
   "已选中"，多次运行的网格互相矛盾）；② 按下前不 hover。修好之后（按下前一帧先 hover、
   `ClipCursor` 钉住光标、逐点 `clean=0/1`、脏样本丢弃）又发现**手势决定结果**：只按下、不移动
   时整个 ±4/±3 窗口（含元件中心）都读 0；带 8 px 拖动时 `(2,0)` 才中。所以旧结论只有两个点
   支撑，命中图还没有跑出来，命中关系仍未定。
3. V1 只公开 `set_footprint` 和 `read_footprint` 仍然正确（放置/占位已真机确认），但不能把它描述
   为 hit box。在做出**可靠且灵敏**的命中读数、并定位真正的命中路径之前，不要增加 `set_hit_box`。
4. （历史状态，已被页首 2026-09-23 追加取代）当时 render 尚未开始写生产接口。

## 已落地的公开接口

服务名与版本：

```c
#define TC_SERVICE_COMPONENT_GEOMETRY "tc.component.geometry"
#define TC_COMPONENT_GEOMETRY_API_VERSION_1 1u

typedef struct TCComponentGeometryApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_footprint)(void* context, uint64_t custom_id,
                         float half_width, float half_height);
    int (*read_footprint)(void* context, const TCGameHandle* component,
                          float* half_width, float* half_height);
} TCComponentGeometryApiV1;
```

语义已经冻结在当前 V1：

- 半宽/半高使用棋盘格单位，宿主向外量化到整数宽高；最小半宽/半高为 `0.5`。
- `set_footprint` 只能在调用方的 `tc_mod_load` 期间使用，且只能修改该 Mod 已注册的 custom id。
- 写入发生在游戏主线程；错误区分 argument、ownership、range、game、thread 等原因。
- `read_footprint` 接受实时 `TCGameHandle`，从游戏当前 Prototype 读取，不从插件声明缓存读取。
- 90°/270° 实例会交换返回的半宽和半高；过期句柄返回 `ERR_STALE`。
- 未调用 geometry 服务的旧 Mod 不触碰 Prototype，行为保持不变。

## 实现落点

| 文件 | 内容 |
|---|---|
| `sdk/tc_service_api.h` | C ABI、版本、错误码和语义注释 |
| `sdk/tc_component_geometry.h` | C++ 查询/调用封装与错误文本 |
| `src/component_geometry.hpp` | 向外量化、原生 64 位矩形打包/解包、旋转半宽计算 |
| `src/native.hpp` | 服务发现、调用方所有权、加载窗口与线程检查、Prototype 安全替换、实时句柄回读 |
| `tests/component-geometry.cpp` | 离线边界、量化、打包和旋转测试 |
| `tests/component-geometry-probe.cpp` | 正式服务真机探针 |
| `tests/component-geometry-playtest.ps1` | 隔离游戏/存档中的 geometry 烟测 |
| `tests/component-hitbox-probe.cpp` | footprint 与鼠标命中的独立对照探针 |
| `tests/component-hitbox-playtest.ps1` | 隔离真机命中回归 |
| `tests/component-hitbox-map-playtest.ps1` | 命中区域网格扫描 + 对照点；本轮证明旧读数不可信 |
| `docs/research/component-hitbox-path.md` | 命中路径静态调用链地图、读数修法与复现命令 |
| `tools/abi-snapshot.cpp`、`abi/windows-x64.json` | ABI 快照 |
| `build.ps1` | `component-geometry.cpp` 纳入本地构建测试 |

探针现在有一个模式开关：`TC_HITBOX_SCAN=map`（由 `component-hitbox-map-playtest.ps1` 设置）会把
两点采样换成 63 点网格 + 5 个对照点，并在按下前插入一帧 hover 预移动。默认（不设该变量）行为
与原来完全一致，`component-hitbox-playtest.ps1` 仍是原来的两点回归。

`component-hitbox-map-playtest.ps1` 暂时**不进 `tests/test-catalog.json`**（和
`component-hitbox-playtest.ps1`、`component-geometry-playtest.ps1` 一样属于研究/诊断脚本）：
它现在断言的是"读数可复现"，而不是某个已经冻结的产品契约；等 §3 的正向对照补上、命中路径定位
之后再决定它是否该进目录。

### 引擎写入方式

不要改回裸指针写入。当前路径是：

1. 用 `custom_prototypes_get` 取得完整 Prototype 副本；
2. 用游戏自己的 Nim 序列析构/分配函数重建 1 项矩形序列；
3. 写入打包矩形和半径；
4. 用 `custom_prototypes_set` 回写；
5. 调用 Prototype 析构函数释放副本。

已确认的当前构建内部布局只封装在 `src/component_geometry.hpp`：形状序列 `+0x48/+0x50`，半径
`+0x58`。插件看不到这些偏移。

## 已通过的验证

### 1. 离线与 ABI

```powershell
& 'C:\msys64\ucrt64\bin\g++.exe' -std=c++17 -O2 -Wall -Wextra -static `
  tests\component-geometry.cpp -o build\component-geometry-test.exe
& .\build\component-geometry-test.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tools\abi.ps1
```

本轮运行结果：geometry 单测通过；ABI 校验通过，共 1120 条记录。

### 2. 正式 geometry 服务真机烟测

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tests\component-geometry-playtest.ps1
```

关键结果：

```text
PASS component geometry service set=0 packed=ok read=0 half=6.000000,3.000000
```

该用例不是只看插件缓存：它注册真实类型，查询服务，调用 `set_footprint(6,3)`，从游戏当前
Prototype 反读原生打包值，经命令总线放置真实实例，再用实例句柄调用 `read_footprint`。

### 3. footprint 与鼠标命中的独立真机对照（**结论已作废，仅留作复现**）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tests\component-hitbox-playtest.ps1 -Seconds 90 -ShotDelay 20000
```

2026-09-22 的通过输出：

```text
hitbox: hit-default  offset=(1,0) ... selected=0 ... want=1
hitbox: hit-default  offset=(2,0) ... selected=1 key=1 want=1
hitbox: hit-expanded offset=(1,0) ... selected=0 ... want=2
hitbox: hit-expanded offset=(2,0) ... selected=1 key=2 want=2
hitbox: scan finished
PASS component footprint is independent from pointer hit testing
```

**这一节的两点对照只在"装置干净"时才算数。** 它的原装置没有隔离真实鼠标输入（`SetCursorPos`
驱动真实光标），测试期间的真实鼠标动作会把采样点顶掉——网格扫描里离元件中心 40 板面格的空点
`(40,0)` 读成"已选中"就是这类污染；另外按下之前没有先 hover。修好装置后（先 hover、
`ClipCursor` 钉住光标、逐点 `clean` 标记、脏样本丢弃）这两点复现，但只按下不移动的手势在整个
窗口里读不到任何选中，所以整张命中图仍要靠带拖动的手势在干净装置下重跑。
完整证据、静态调用链与修法见 `docs/research/component-hitbox-path.md`。

探针的实现细节很重要：

- `world_pos_to_screen_pos` 返回归一化坐标，先乘 ImGui `DisplaySize`，再按 Win32 client 尺寸做
  DPI 映射；早期直接把 display 坐标发给窗口的结果无效。
- 按键使用游戏同一 ImGui IO 的 `ImGuiIO_AddMouseButtonEvent`，DOWN 保留整帧；单纯连续
  `PostMessage(WM_LBUTTONDOWN/UP)` 会在游戏采样前折叠掉状态。
- `select_component__modelZboardZboard_u9202` 的选择 key 是**元件序列索引**，不是元件记录内的
  64 位实例 id；探针已按序列索引比较。
- 每个样本调用 `clear_selections__modelZboardZboard_u8323`，并清当前/previous component set。
- 日志中 expanded `(1,0)` 的旧 key/snapshot 仍可能短暂显示上一样本，因为 release/action 的处理
  跨帧；最终断言比较的是“目标 key 是否被选中”。当前四个对照点稳定通过，但若要测精确边界，
  最稳妥的升级仍是**每个点启动一个新进程/新棋盘**。

## 已做的引擎研究

以下结果可直接承接，不必重新猜签名：

- `clear_selections__modelZboardZboard_u8323`：无参数，清游戏全局选择。
- `select_component__modelZboardZboard_u9202`：调用点显示第一参数是 component sequence index，
  第二参数是 component record 指针。
- `compute_bounding_boxes__modelZboardZboard_u5576`：对 24/8 字节矩形序列做通用包围盒计算，
  目前没有证据表明它直接决定主元件鼠标命中。
- `compute_bounding_boxes__presenterZutilitiesZinput95utilities_u13751`：presenter 侧通用矩形并集。
- `get_interactables_rect__presenterZutilitiesZinput95utilities_u16102`：custom 分支遍历内部元件，
  对 kind `0x5a` 的特殊 interactable 矩形做旋转与并集；更像子控件交互区，不是主元件拖拽体。
- 后续值得继续看的符号：
  `get_interactables__presenterZutilitiesZinput95utilities_u15450`、
  `init_interactables__...`、`handle_hover_interactables__...`、
  `handle_press_interactables__...`、
  `select_component__presenterZutilitiesZhelper95functions_u4505`、`shape_contains__...`。

## 已证伪或危险的路线

- 把内部电路做宽：compact（内部宽 9）与 wide（内部宽 117）的 32×32 设计图和放置拒绝范围相同，
  游戏会把内部电路自动缩放，不能借此得到文本框大小的命中区。
- 给 V1 增加 `set_hit_box` 但仍写 shape sequence：真机对照已经证明语义不成立。
- 插件直接写 Prototype `+0x368/+0x508` 或 `+0x570` 设计缓冲区：既有探针会让游戏在加载期直接
  退出，`fault.log` 甚至来不及记录。必须由加载器走游戏自身的复制/更新/析构链。
- 用空内部定义隐藏默认图形：确实不再绘制游戏像素，但同时失去可点击区域，不能满足“隐形但
  整框可拖”。

## 下一步建议（按顺序）

### A. 先修好命中读数（本节是上一条 B 的前置条件）

读数的两个要求必须同时满足，缺一不可：

1. **灵敏**：在已知可选中的目标上必须能读到 1。先用原版元件/原版交互确认这套合成输入确实能触发
   选择；连"能选中"都测不出来时，任何 0 都不说明问题。
2. **稳定**：同一点重复三次结果一致；离元件 ≥8 格的空点读到 0（`(40,0)` 对照点已经做到）。

现在的状态：装置已修好（按下前一帧先 hover、`ClipCursor` 钉住光标、逐点 `clean=0/1`、脏样本
丢弃并让用例判失败）。已知的事实：

- 只按下、不移动的手势 → 干净运行里 63×2 网格全 0（含元件中心）；
- 按下 + 8 px 拖动（旧用例手势）→ 干净运行里 `(1,0)`=0、`(2,0)`=1，两种 footprint 相同；
- 用同一拖动手势跑整张网格 → 本轮被污染检测判废（136 点里 18 点被真实输入顶掉）。

所以还缺一次**干净的全网格运行**；之前几次的抖动已归因于未隔离的真实输入，不再算作引擎行为。
每次运行的地图会追加到 `build/hitbox-map-out/map-history.txt`。
文档措辞（`sdk/tc_service_api.h`、`docs/sdk/services.md`、`docs/reference/limits.md`、
`docs/PLAN-custom-components.md`、`docs/changelog.md`）本轮已按"命中关系未决"改正。

### B. 找到真正的主元件 hit 路径，再设计 geometry V2

从 presenter 的 press/hover 调用链向下跟 `select_component`，记录最终用于主元件的矩形/形状来源；
`docs/research/component-hitbox-path.md` 已给出可直接承接的静态地图（交互区 R\* 树、自定义元件只
贡献内部 kind 0x5a 元素、`shape_contains` 只被走线调用、高亮几何来自 renderer mesh 等）。
只有满足以下条件才增加 `set_hit_box`：

- 默认与扩大后的类型在同一隔离用例中出现不同命中结果；
- 旋转 0/1/2/3 都通过；
- 不破坏 pin、特殊 interactable、框选和拖拽；
- 写入仍使用游戏自己的分配/复制路径；
- 旧 V1 的结构大小与行为不变，新增接口使用 V2。

### C. `tc.component.render` 最小切片（已完成）

V1 已按以下范围冻结并通过离线 ABI/构建与文本框真机回调烟测：

1. 按类型注册实例绘制回调；
2. 宿主枚举当前棋盘实例，提供 handle/custom id/instance id、局部到屏幕仿射基、旋转和裁剪矩形；
3. 复用 `tc_ui_draw.h` 已验证的 ImGui draw-list 原语，不把 renderer 私有指针长期交给插件；
4. 回调只在棋盘渲染帧有效，离开棋盘立即停止；
5. 先验证覆盖层绘制，再单独研究安全的“关闭游戏默认绘制”路径，不要把两者绑成一次高风险提交。

可复用的参考实现是 `examples/text-box/plugin.cpp`：它已包含 board→screen 仿射计算、主视口
background draw list、字体替换、文本布局和按实例枚举。但这些现在都由插件自己做，M5 render 的
目标是把稳定部分收回宿主服务。

### D. 迁移文本框并完成 M5（下一步）

文本框已是正式消费者：8×4 footprint + render 回调边框已经过真机。下一刀是把复杂字号/斜体正文
也迁入服务（需要字体资源切片）；随后补默认绘制关闭、缺 Mod 可见占位、
纹理/字体等资源的场景切换释放，并用移动、四向旋转、缩放、裁剪、离开关卡后的回调计数和像素
回读做验收。整框拖动已经由 footprint 提供，不再需要独立 hit box。

## 当前工作区注意事项

仓库在本轮开始前就有大量已修改和未跟踪文件，它们属于同一长期开发工作区。不要用
`git reset --hard`、`git checkout --` 或批量清理命令。M5 相关文件也大多尚未单独提交；接手时应按
上面的文件表逐个审阅，只暂存明确属于本阶段的改动。

上一轮最后一个确定通过的命令是 `tests/component-hitbox-playtest.ps1`（**现已判定为不可信**）。
本轮最后一个确定通过的命令是 `tests/component-hitbox-map-playtest.ps1`（读数稳定性 + 对照点），
它同时给出了"旧两点结论无效"的证据；本轮没有写 render 生产代码。
