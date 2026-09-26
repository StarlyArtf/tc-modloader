# 自定义元件的"图标"是怎么画出来的（2026-09-26 实测）

目标：搞清楚游戏给**自定义元件（含 Mod 元件，kind `0x4e`）**在元件栏里画的那张小图从哪来、能不能接管。
探针 `tests/icon-probe.cpp` + 用例 `tests/icon-playtest.ps1`（真机、只读 hook + 进程内 GL 截图）。

## 1. 画图标的是谁

元件栏每一项由这三个函数之一画出（按页面形态分派，都从 `presenter/board_ui/component_menu`）：

| 函数 | 用途 |
|---|---|
| `build_component__presenterZboard95uiZcomponent95menuZmini95tree_u336`（VA `0x1403cce60`） | 迷你树（右侧栏）的一项 |
| `build_component__…Zflat95list_u119`（`0x1403cc130`） | 扁平列表的一项 |
| `build_component__…Zdeep95tree_u300` / `u3390` | 元件工坊的深层树 |

每个都做同一件事：**先要一张纹理，再把项目画上去**

```text
get_captured_path__presenterZio_u28(out, kind, value, flag)   ; kind==0x4e 走自定义分支
  └─ 自定义：u25 → "?snapshot_cc/com_custom_" & $value & ".png"  → get_asset_path()
  └─ 内置：  u15 → "?snapshot/" & $kind（kind 名，如 com_screen） ".png" → get_asset_path()
create_texture_unsafe__presenterZrendererZtextureZtexture_u364(out, path, …, …)
create_texture__presenterZrendererZtextureZtexture_u1978(out, path, …)   ; 上面那个的检查版包装
```

`get_asset_path` 只是把 `?` 去掉后拼到 `<游戏目录>/asset/` 上，所以游戏实际要的路径是：

```text
<游戏目录>/asset/?snapshot_cc/com_custom_<十进制 id>.png      ← 自定义元件（本 Mod 的类型）
<游戏目录>/asset/?snapshot/<内置 kind 名>.png                 ← 内置元件
```

真机日志（FP32 Constant，`0x463332434f4e5331` = 5058442071141929777，开抽屉或有元件栏时逐帧请求）：

```text
ICON-PROBE: texture request "component_sprites/com_off.png"
ICON-PROBE: texture request (unsafe) "…/game/asset/component_sprites/com_off.png"
ICON-PROBE: texture request (unsafe) "…/game/asset/?snapshot_cc/com_custom_5058442071141929777.png"
ICON-PROBE: texture request (unsafe) "…/game/asset/?snapshot_cc/com_custom_5058442071108509489.png"   ← FP32 Add
```

内置元件的外观表见 `tools/component-sprites.js`（`asset/component_sprites/com_*.png`），而
**目录缩略图**是另一张：游戏自带 698 张 `asset/capture/*.png`，其中 `com_custom_<id>.png` 就是它自己那些
自定义元件的缩略图（按**十进制 id** 命名，和请求里的数字对得上）。

## 2. 放一份 PNG 能不能接管？（实测：**不能**）

用例把一张品红色 64×64 PNG 放到游戏请求的那条路径上（`asset/snapshot_cc/com_custom_<id>.png`，
同时把绿色放 `asset/capture/` 作对照），跑真机、抓到元件栏展开后的帧：

* 游戏**确实读了它**：运行后该文件被游戏重新编码（1060 字节 → 642 字节），像素仍是那张品红图；
  请求行也在日志里逐帧出现；
* 但**画出来的图标不是它**：把"放了品红文件"与"完全不放文件"两次运行的第 9 帧逐像素比对，
  元件栏区域（x 1400–2536、y 380–720）**差异 0 像素**——两次完全一样；
* 也就是说：图标是游戏**自己渲染**出来的（`component_custom` 那套设计图/水印的离屏渲染，
  相关符号：`render_component_snapshots__main_u414`、`update_dirty_snapshots__main_u873`、
  `is_snapshot_mode__presenterZrendererZmulti95meshZmulti95mesh_u270`、
  `presenter/component_snapshot/snapshot.nim` 的 `get_snapshot_rect`），`?snapshot_cc/...png` 这条
  路径是它的**缓存/回退**通道，不是元件栏画的那张。

## 3. `shape_svg` 能不能接管？（实测：**对自定义原型无效**）

内置元件的缩略图来自原型里的 `shape_svg`（`+0xb0`，SDK `TCComponentTypeDefinitionV2::shape_svg`，
加载器 `src/native_component.hpp` 已经会 `setPrototypeShapeSvg`）。用探针把几个内置原型的
`builtinPrototypeShapeSvg()` dump 出来，格式是一段**内联 SVG 片段**，坐标就是元件本体的局部坐标，
每个图元带 `class="component"`：

```text
kind=0x03 NOT bit       <path d="M -12 -18 L -12 18 L 33 0 Z" class="component"/>
kind=0x5a Static Value  <rect x="-50" y="-30" width="100" height="60" class="component" />
kind=0x4f Input Pin     <circle cx="-10" cy="0" r="60" class="component" />
```

给本 Mod 的 FP32 Constant 加上一段特征 SVG（`rect` + `circle`）后重跑同样的用例：元件栏区域与
不加的版本**同样 0 像素差异**。这与 `docs/PLAN-custom-components.md` 早先记下的
"`shape_svg` 无影响"一致——**游戏不为自定义原型使用 `shape_svg`**，它只影响内置元件。

## 4. 结论：要接管只能挂渲染，不能只给数据

### 4.1 又试了一条"数据路线"：改设计图缓冲区（实测：能改到棋盘，改不到卡片）

设计图是原型 `+0x570` 上的一条 Nim seq：**64 个 64 位字 = 1024 个 4bit 格子**（每格
`value = 低 4 位`、`color = 高 4 位`，`component_custom.frag` 的读法），前面 8 字节是 seq 的
容量/长度头（实测 `header=64`）——上一轮的"裸偏移手术"崩溃就是把这 8 字节一起涂了。

探针（`TC_ICON_PROBE_DESIGN=<id16>`）现在做的是**安全版**：`getCustomPrototype` 克隆 → 只写
`buffer+8` 起的 512 字节格子（头部不动）→ `setCustomPrototype` 回写 → `releasePrototype`
（加载器导入自己的那条往返）。实测：

```text
ICON-PROBE: design: id=0x463332434f4e5331 buffer=… header=64 cell=0x51 cells written
ICON-PROBE: design: set=ok release=ok (model=ok)
```

不回写（只改克隆）**完全没有变化**（克隆是真拷贝）；回写之后**棋盘画面变了**（第 9 帧与基线
逐像素比较：棋盘区 244 个采样点不同），而**元件栏卡片区 0 像素差异**。

也就是说：设计图缓冲区是"元件在棋盘上那张图"的来源之一，但**元件栏卡片/抽屉预览画的不是它
当时的缓冲区内容**——这两个面都在逐帧向渲染器请求同一张"快照纹理"
`?snapshot_cc/com_custom_<id>.png`（第 1 节的请求行），并且在请求前后对纹理做了自己的处理
（放文件会被读、会被重新编码，却不会改变画出来的图）。

### 4.2 结论

#### 4.2.0 拦截点的现状（静态）

`create_texture_unsafe(out, path, …)` 的 out 结构写入形态（反汇编尾部）：

```text
[out+0x00]  8 字节（一次 movq/movups 写入，像"句柄或像素指针"）
[out+0x08]  u32        ← 与 +0x0c 成对，形态上是宽/高
[out+0x0c]  u32/byte
[out+0x10]  u32
```

即纹理结构约 20 字节。自己合成这个结构前必须确认它的语义（尤其是 +0 那 8 字节与析构路径），
否则游戏释放时会踩内存；这也是"拦截后返回自建纹理"这条路线当前唯一没落实的技术点。
下一步先用只读探针把两件事测掉：(1) 工厂返回结构里哪些字段被绘制/析构读到；(2) 同一路径逐帧
请求时缓存命中发生在工厂之前还是之后（决定要每帧替换还是替换一次）。

#### 4.2.1 Mod 侧的画面来源（玩家 2026-09-26 定的）

**Mod 侧直接用元件外观当缩略图**：加载器把该类型的**棋盘绘制回调**在离屏目标里跑一遍（给它一个
映射到缩略图尺寸的 local→screen 变换），把结果作为这张"快照"纹理交给游戏。这样每个类型自动拥有
与棋盘一致的外观（本体色、`32` 徽标、名字、中间符号、引脚），不需要 Mod 另画 32×32 素材；
没有注册回调的类型仍旧回退到游戏自己的设计图渲染。

测量结论：

1. 自定义元件的图标 = 游戏把这个原型**渲染**成一张小图（设计图缩略图 + 名字水印），画进元件栏/抽屉；
2. 渲染结果在 `?snapshot_cc/com_custom_<十进制 id>.png` 这条路径上被读取/回写（放文件会被读、会被
   重新编码，但**不会**改变画出来的图标）；
3. `shape_svg` 对自定义原型无效；`set_default_drawing(custom_id,false)` 只关**棋盘上**的默认绘制
   （`TCComponentRenderApiV2`），不影响元件栏图标。

因此接管需要加载器提供一个新的**渲染级**能力。当时列出的两条路线如下；路线 A 后来已经作为
render V5 落地，放置幽灵则由独立的 render V6 解决：

| 路线 | 挂点 | 语义 |
|---|---|---|
| A. 纹理替换 | `create_texture__u1978` / `create_texture_unsafe__u364`，匹配 `?snapshot_cc/com_custom_<本 Mod 的 id>.png` | 加载器用 Mod 给的图回答元件栏卡片和抽屉预览的纹理请求；后续真机复测证明放置幽灵不消费这张图，因此它只覆盖两个面 |
| B. 直接改设计图 | `+0x570` 的 512 字节格子（头部不动）+ `setCustomPrototype` 回写；若游戏重算（`update_custom_design`）需在之后重涂 | 已实测可写、可生效（棋盘画面确实变了），但**不改变**元件栏卡片，所以单靠它满足不了三个面 |

路线 A 更小、更可回退；最终实现没有手工合成纹理句柄，而是把加载器副本路径交回游戏自己的
`create_texture_unsafe`，继续由游戏构造、缓存和释放纹理。

## 复现

```powershell
# 真机：放一张特征色图标 → 悬停展开元件栏 → 读回游戏请求的路径 + 逐像素比较
& .\tests\icon-playtest.ps1                 # 带图标文件
& .\tests\icon-playtest.ps1 -NoIcon         # 对照：完全不放文件
# 产物：<沙盒>/game/tc-modloader-data/plugin-data/test.icon-probe/{icons.txt,palette-N.bmp}
```

两个细节值得记住：元件栏的层级是**悬停展开**的（分类页签 → 子分类飞出层 → 元件飞出层，
`TC_ICON_PROBE_PALETTE_Y`/`RIGHT` 控制点击位置），而 1200×800 的小窗口下游戏**不画**元件列表，
所以用例默认用玩家自己的窗口尺寸（2536×1452）。

## 5. V5 落地之后的复测（2026-09-26 晚，`tc.component.render` V5）

`sdk/tc_service_api.h` 的 V5（`set_picture(context, custom_id, png_path)`）与加载器的拦截已经落地，
这一节是"接管到底覆盖哪几个面"的复测。**结论和本文件第 4 节里的假设不完全一样**，以本节为准。

### 5.1 三个面各自实测（用例 `tests/picture-playtest.ps1`，探针 `tests/icon-probe.cpp` 的 stage 流程）

流程：进关卡 →（1）悬停展开元件栏到"浮点"元件列表（卡片）→（2）点元件本体开底部抽屉（抽屉预览）
→（3）再次展开列表、把里面的一项**按住拖到棋盘上**（放置幽灵）。每一面各拍 2–4 帧进程内 GL 截图，
然后同一套流程再跑一次**关掉图片注册**的对照（`TC_FLOATOPS_PICTURES=0`）。

| 面 | 注册了图 | 关掉注册（对照） | 结论 |
|---|---|---|---|
| 元件栏卡片（22 项一排） | 品红标记图占满一项一项的位置 | 全是游戏自己画的缩略图 | **V5 生效** |
| 底部抽屉预览 | 品红标记图 | 游戏自己画的缩略图 | **V5 生效** |
| 放置幽灵 | 两个引脚 + `FP32` 名字标签 + 连线预览，**没有本体** | 同上，逐像素一致 | **V5 不覆盖这个面**；后来由 V6 独立接管，见 §5.2 |

幽灵这一条用四种输入各测一次，结果完全一样：随包的真图（192×192）、品红标记图（64×64 / 160×160 /
192×192 三种尺寸）、以及完全不注册。也就是说：**这个构建里自定义元件（kind `0x4e`）的放置幽灵
只有引脚、名字标签和连线预览**，本体那一步游戏没有画（棋盘上的本体是加载器交给 Mod 的绘制回调画的，
而幽灵不是棋盘实例，原有回调不会为它触发）。这是 V5 的边界；后续 V6 在专用的 clipboard redraw
路径捕获临时记录并主动调用同一个回调，不再需要改 32×32 设计图。

### 5.2 V6：专门接管放置幽灵（2026-09-26）

最终使用的挂点是 `redraw_clipboard_component__presenterZupdate_state_clipboard_u7`：它收到 id-less 的
custom component 记录，记录内已有吸附后的格点坐标、旋转和 custom id。V6 按类型选择接管后，加载器
跳过游戏的默认幽灵，并以 `instance_id == 0`、零组件句柄、空配置调用该类型现有的绘制回调。因为 ImGui
绘制是即时的，而游戏的 clipboard mesh 是保留式的，捕获结果会每帧重复提交；结束放置的信号取自游戏
自己的剪贴板**记录生命周期**，见 §5.3（`hide_clipboard` 只作兜底：成功放置那条路根本不调它）。

`tests/picture-playtest.ps1` 的真机结果：拖出 `FP32 Absolute` 后连续四张 GL 截图都出现完整紫色
`ABS |x|` 本体与红色引脚；开启/关闭 V5 图片注册时结果相同，且幽灵区域的品红标记像素始终为 0。松开
鼠标后的一帧落点附近紫色为 0，把元件真的放到棋盘上之后那一格只剩棋盘实例本体。这同时证明 V6 接管
持续有效、V5 图片与 V6 幽灵是两个互不混淆的渲染面，以及结束信号真的被认出来。

### 5.3 松手后幽灵残留：结束信号是记录，不是可见性（2026-09-26 夜，已修）

V6 上线后的第一个真机 bug：**放完元件，幽灵不肯消失**（玩家报告）。原因不是捕获错了，而是“放置结束”
只挂在 `hide_clipboard` 上——游戏存在不调它、只把记录放下的路径，于是即时绘制的预览留在最后一个吸附
格点上。修的过程里有两条被实测否掉的候选信号，记下来免得下次再走：

| 候选 | 实测 | 结论 |
|---|---|---|
| `is_clipboard_visible__presenterZcontext_u2911`（`return *(u8*)(ctx+0x25)`） | 按住元件拖动的**整个过程里返回 0**；反汇编显示游戏自己在这条 `0` 上走“不可见”分支，却仍然把幽灵画出来 | 不是“幽灵是否可见”，用它会一帧都不画 |
| `redraw_clipboard_component` 的调用间隔（“不再调用即结束”） | 一次拖动里只被调用 5 次（按下、每次换格、静止时偶发），静止等待与结束无法区分 | 只能用来**开始/更新**预览，不能当结束信号 |
| `update_state_clipboard__presenterZupdate95state95clipboard_u120` 交出来的记录 | 剪贴板每次变化都被调用（暂停的棋盘上也在按下/换格/松手时各来一次），`arg3` 就是那块 0x238 字节的记录：拖动中 `+0=0x4e` 且 `+0x188` 是本类型的 custom id，松手后同一槽位变成 `+0=0`（另一条路径下游戏换到另一个空记录槽） | **这就是结束信号**：kind 不是 `0x4e` 或 custom id 换了，就说明游戏已经把剪贴板放下 |

探针（`tests/icon-probe.cpp`）现在跑两次拖放：一次松手在元件栏上（取消），一次**真的放到棋盘上**
（成功放置）。两次都在加载器日志里留下 `placement preview taken over` → `placement preview ended`
成对的行，且 `stage after`（取消后）与 `stage settled`（放置后）两帧里取消落点附近的紫色都是 0。
用只挂 `hide_clipboard` 的上一版加载器跑同一脚本：`after` 一帧落点是 `Body=424`、`Pins=3`——
残留的幽灵，正是玩家看到的那一个。

### 5.4 两个坑（都在真机上撞过）

1. **游戏会重写它读过的那张 PNG**。把替代路径直接指给 Mod 自己包里的文件，游戏读完会把重编码结果
   **写回同一个路径**（实测 1060 字节 → 642 字节）。Mod 的文件是加载器在管的（`state.json` 的
   `files` 记录里有哈希），被外部改写之后下一次 apply 会拒绝并报 "File changed outside loader"。
   所以加载器**先把图复制到自己的目录**（`<game>/tc-modloader-data/pictures/<十进制 id>.png`）再把
   副本路径交给游戏：游戏爱怎么写就怎么写。
2. **Nim 字符串要活过这次调用**。给工厂的替代路径是 `{length, payload}` 形式，payload 的字符在
   `payload+8`（同一个约定见 `src/save_boot.hpp`）。这份内存在**栈上**时，游戏卸载那张纹理时会再读
   一次，就成了访问越界（`fault.log` 里 "outside any plugin callback: access violation"）。现在这份
   payload 和副本一起存在加载器的表里，进程活多久就有效多久。

### 5.5 元件栏那一排卡片的几何（顺手量到的）

元件列表不是竖排也不是网格：22 项被画成**一条横排**，右端贴住面板左缘（实测 2536×1452 窗口下
`x 480..2356`、`y 548..624`，一项约 85×76 px），每项是一张 `ITEM_SIZE`(60×60) 的图加名字。
探针因此不再猜坐标：它从自己刚拍的那帧里找**最右边的一块特征色**（标记图），拿它的重心当"这一项的
位置"再按下去——这样换窗口尺寸或换图片尺寸都不会点错（`analyseItem`）。

### 5.6 复现

```powershell
# 真机：图片（品红标记）→ 三面截图；再跑一次关掉注册的对照；用例自己比色
& .\tests\picture-playtest.ps1
# 不发包、只看随包那 22 张图长什么样（离线，不需要游戏）
& .\tests\float-icons.ps1
# 产物：<沙盒>/…/plugin-data/test.icon-probe/stage-{green,red}-{card,drawer,ghost}-N.bmp
```
