# 日志与诊断速查

## 日志位置

| 文件 | 位置 | 内容 |
|---|---|---|
| 加载器日志 | `<游戏目录>/tc-modloader-data/loader.log` | 加载器、插件、各子系统日志 |
| 上一份日志 | `<游戏目录>/tc-modloader-data/loader.log.1` | 超过 8 MiB 时轮转一次（只保留上一份） |
| 崩溃归属 | `<游戏目录>/tc-modloader-data/fault.log` | 插件回调里的硬件异常：哪条链节/监听器、什么原因（见下） |
| 插件数据 | `<游戏目录>/tc-modloader-data/plugin-data/<mod id>/` | 插件自己写的数据（例如自动验证开关） |
| 生成源码转储 | 游戏工作目录 | `native-logic-source-<n>.txt`（每次编译一份）、`native-logic-source.txt`（含桥接调用的那份） |
| 隔离沙箱日志 | `build/<playtest>-<guid>/game/tc-modloader-data/loader.log` | 自动验证每次运行的副本 |
| 版本不匹配标记 | `<游戏目录>/tc-modloader-data/unsupported-build.txt` | 加载器版本 + 游戏 EXE/引擎哈希；出现它就说明这次运行没有真正接管（见下方"与游戏不匹配时"） |

## 与游戏不匹配时（0.8.0 起）

`loader.log` 里会出现这三行的组合，它们就是"游戏更新了，请换匹配的加载器"的机器可读形式：

```text
Save isolation unavailable: <原因>
This session runs with no save redirect and no native plugins; resource Mods still apply
Unsupported game build. Reinstall a compatible loader.
```

同时会弹一次原生对话框（同一"加载器版本 + 游戏哈希"只弹一次），并写下
`tc-modloader-data/unsupported-build.txt`。这种情况下主菜单没有 Mods 按钮是预期行为：加载器知道的
对齐点已经不可信，游戏内页面可能根本不会被调用，所以提示走的是 Windows 对话框。

## 日志前缀

| 前缀 | 来源 | 说明 |
|---|---|---|
| 无前缀 | 加载器 | 启动横幅、存档路径、Hook 装载、子系统状态 |
| `[<mod id>]` | 加载器转发插件日志 | 插件调用 `host->log` 的内容 |
| `[<mod id>] status:` | 插件 | 插件通过 `report_status` 报给 Mods 页的同一行（文本变化才写，避免逐帧刷屏） |
| `Native logic:` | 原生逻辑子系统 | 注册、绑定、发射、重置、诊断 |
| `Component timing:` | 声明统计子系统 | `Mod declared cost id=.. gates=N delay=M`（登记时捕获）、`native timing retained...`（未验证形状回退）、`gate total override skipped...`（异常兜底，最多一次） |
| `cost-watch:` | `dev.cost-watch.mod` | 界面分数、各 kind 代价、原型字段 |
| `custom-or:` 等 | 示例插件自身 | 示例的行为日志（可用于断言） |

启动时还会写两行"这个加载器是什么、能提供什么"，排查"插件说宿主不支持 X"时先看它们：

```text
TC Mod Loader 0.6.0; ImGui 1.92.6
TC Mod Loader 0.6.0 capabilities: log, symbol, hook, logic, component, ui_page, ui_slot, texture, status
```

能力名字与 `mod.json` 的 `capabilities` 是同一张表（见
[capabilities.md](capabilities.md)）。扫描期的两种拒绝也要分清：

| 日志 / Mods 页文字 | 含义 | 处理 |
|---|---|---|
| `Unknown loader capability: X` | 包写错了名字 | 改包；加载器不用动 |
| `This loader does not provide the capability: X` | 名字存在，加载器太旧 | 升级加载器 |
| `X requires Y >=1.2.0, but the enabled version is 1.0.0` | 依赖版本不满足（**没有文件被改动**） | 换版本或改包 |

## fault.log：插件崩了，是谁

原生插件和游戏同进程，一个野指针就会带走整个游戏。加载器**不做崩溃恢复**（试过并放弃，
原因与实测见 [verification.md](../verification.md#安全模式与崩溃隔离)），但它会给崩溃**署名**：

```text
dev.hook-chain-crash in sim.do: access violation at 0x0000000000000008
```

一行里三个信息：Mod id、当时在跑的回调（`on_frame` / `sim.do` 这样的钩子点 / `an event
listener`）、异常类型与访问地址。玩家只要把这个文件发过来，就不必猜"是哪个 Mod 干的"。
日志同样会轮转（8 MiB，上一份是 `loader.log.1`），所以长期挂机也不会写出一个巨大的文件。

## 左侧 IO 面板（引脚顺序 / 缓存同步）

面板画的是缓存下来（只在关卡加载时重建）的三段序列，所以"某个引脚的框不见了"要分三类看。
把 `TC_MODLOADER_PIN_ORDER_LOG` 设成 `1`（`dump` 再加原始结构），日志里会出现：

| 日志 | 含义与后续动作 |
|---|---|
| `IO panel armed (cache rebuild after a prototype reload + tc.pin_order)` | 加载器拿到了面板构建入口：改引脚后重建缓存 + 套用 Mod 的顺序。这一行不在，两个功能都没接上 |
| `IO panel cache rebuilt (a custom prototype was reloaded)` | 工坊里改名/增删引脚后，缓存真的重建了（"不用退出再进工坊"就是靠它）。玩家手动改一次引脚就应该出现一行 |
| `IO panel cache rebuild failed: the game's io state helpers are missing` | 三个游戏函数地址没取到：兼容画像不对，面板会退回"要退出再进"的旧行为 |
| `pin order: summary \| inputs cache=N rows=N frames=N \| …`（逐帧变化时才打印） | 缓存条数 / 本帧画了几条 / 画了几个框。`cache` 比面板显示的少 = 缓存旧；`rows` 比 `cache` 少 = 锚点没认出来；`frames` 比 `rows` 少 = 框被下一条目或裁剪吃掉 |
| `pin order: row <组> #i key=… width=… top=… bottom=…` | summary 变化时每条的几何，用来核对框的位置与高度 |
| `pin order: panel anchor +0x… is not handled` | 面板里出现本 Mod 不认识的锚点（工坊第三组"内存/寄存器"就是这一类）；拿到这个偏移就能补测量 |
| `pin order dump: group N count=… entries=…` / `group N unavailable count=… contextReadable=… header=…` | 原始缓存结构。`contextReadable=0` 或 `header` 与 `count` 不一致时，服务选择不动作而不是猜 |

## 原生逻辑关键行

| 日志 | 含义与后续动作 |
|---|---|
| `Native logic: registered custom 0x.. inputs=N outputs=M in0=(x,y,wB) ... shape=..` | 注册成功。核对引脚数、每脚位宽与实测偏移是否与定义一致 |
| `Native logic: registration rejected: ...` | 引脚数或位宽超出上限（见 [limits.md](limits.md)） |
| `Native logic: bound instance 0x.. of custom 0x.. as token N` | 编译时发现该实例并建立绑定 |
| 只有 `bound` 没有 `emitted` | 元件内部节点不满足形状规则，回调不会运行（看下一行的诊断） |
| `Native logic: instance ... has no recognised internal logic node` | 内部节点不在识别范围（多级逻辑、RAM/寄存器等） |
| `Native logic: instance ... exposes N operand(s) but the definition declares M input(s)` | 第一个输出门的操作数与定义输入脚数不符；日志会附带原样发射行 |
| `Native logic: emitted N callback(s), mode=0/1` | 运行（0）与刷新（1）两个模式各替换的行数 |
| `Native logic: board layout nodeI=0x.. parent=0x..` | 进程首次编译的扁平化板面；用于确认元件内部节点 kind |
| `Native logic: reset N instance(s)` | 游戏重置路径触发 RESET |

## 常见故障定位

| 现象 | 先看什么 | 常见原因 |
|---|---|---|
| 插件没加载 | `插件名 failed: ...` 或管理页状态 | 包未应用/指纹不匹配、ABI 版本不符、DLL 缺依赖 |
| Hook 没生效 | `Hook rejected: phase, target or conflict` | 目标不是 EXE 函数或原引擎可执行导出、目标已被其他 Mod 占用、在入口之外注册 |
| 回调一次都没跑 | 有无 `bound` 与 `emitted` | 元件没放到板上、原理图引用的 ID 不同、形状规则不满足 |
| 关卡判定与日志不一致 | `exposes ...` 诊断行 | 元件内部节点的输入元组不是定义引脚；或原理图复制了关卡自带 IO |
| 字宽输出关卡读到 0/空白 | 生成源码转储里的输出赋值行 | 输出脚没接到关卡输出（连线坐标错一格）、位宽不匹配 |
| 编译器报 `register_frame.nim(119,3) ... == EXP_NULL` | 生成源码转储的调用行 | 给生成代码发了超过 4 个参数的外调（插件不应直接这么做） |
| 延迟统计没按声明值 | `Component timing:` 行 | 板子含反馈环/多驱动/收缩后成环，回退到原生统计 |

## 生成源码转储怎么用

## 界面文字：先问"当前字体是谁"

ImGui 的文字要有字形才会画顶点：字体不对时**矩形照画、文字全无**。加载器有一对环境变量
和一条常驻日志专门回答这个问题：

| 工具 | 用法 | 得到什么 |
|---|---|---|
| `TC_MODLOADER_TRACE_TEXT` | `$env:TC_MODLOADER_TRACE_TEXT='TOOLFONT'`，再启动游戏 | 钩住 `igRenderText`，凡文本里含该子串的提交都写一行：位置、字体名、字号、当前窗口绘制列表的顶点数 `before -> after`。`after` 不增长就说明字形压根没产生（字体不对） |
| `Tool column text font: defined_fonts[N]=<字体名> (used for plugin tool draws)` | 加载器启动时自动写 | 插件工具栏控件借用的是游戏哪一份正文面（本构建 `defined_fonts[2]=NoroshiCode_Regular.ttf`） |
| `tools/scan-calls.js` | `node tools/scan-calls.js <带导入表的dll> <要扫的exe> <名字正则> <RVA起> <RVA止>` | 反查 EXE 里哪些位置调用了某个导入（含 `call/jmp [rip]`、`E8` 直达、跳转桩），用来抄游戏自己的做法，例如它在工具栏里怎么 `igPushFont/igText` |

注意：探针绘制内容要**每帧都画**——只在"每 N 帧"画一次的探针会让定时抓帧抓到空帧，
从而误判成"文字没显示"。

加载器把每次编译的中间源码写到游戏工作目录：

- `native-logic-source-<n>.txt`：按编译顺序编号（一个关卡会分别编译关卡 IO 程序与原理图程序）。
- `native-logic-source.txt`：含桥接调用的那一份，便于直接看替换结果。

典型排查路径：在转储里搜 `tc_logic` 看替换后的行，再搜 `com_` 找到对应元件节点与
`level_output.` 赋值行，对比游戏自己的发射形式。示例：

```text
// 5 com_not_word 8 ...            <- 元件内部节点（占位门）
var tc_io1 = U1 game_engine.'tc_logic_invoke'(U64 1, U64 (cycle + 1), U64 (vid258), U64 0)
var vid260 = U8 ((((U8 (load(<U1>, #SIMULATION_STATE + 10092544))) & 1)) | ...)
```

## 隔离沙箱

## 看加载器自己的界面（开发用）

加载器的页面（主菜单的 **Mods** 管理页）画在游戏自己的 GL 窗口里，而这台机器的窗口是
**独占翻转**的全屏窗口——`PrintWindow` / 桌面截图只能拿到纯黑。改界面时用这两个环境变量：

```powershell
$env:TC_MODLOADER_OPEN = '1'                                  # 启动后自动打开管理页（免点击）
$env:TC_MODLOADER_OPEN_SAVES = '1'                            # 免点击打开「存档」页
$env:TC_MODLOADER_SHOT = 'D:\shot\mods.bmp'                   # 打开若干帧后写一张 BMP
```

`TC_MODLOADER_SHOT` 走的是进程内 `glReadPixels`，拿到的是**两帧前**的画面（管理页已经画好），
日志里会写 `Captured manager screenshot`。两个变量都不设时对正常游戏没有任何影响。

要看**关卡里**的界面（板侧栏面板、导线调色盘、电路本身），管理页那套触发不够用——菜单页的
绘制只在主页运行。改用定时抓帧，配合只会“进入关卡”的探针 `dev.enter-board`：

```powershell
$env:TC_MODLOADER_SHOT       = 'D:\shot\board.bmp'
$env:TC_MODLOADER_SHOT_DELAY = '14000'                        # 毫秒
```

抓帧在 `igEnd` 里按时间触发（每帧都会走），日志写 `Captured frame screenshot`。

要拿**元件自己的排版**跟截图对账（本体多大、字多高、引脚在哪），浮点元件族有一个按帧输出的
诊断：设 `TC_FLOATOPS_LAYOUT=1` 再启动游戏，`local.float-ops` 每个实例每帧写一行

```text
float-ops render: type=0x463332434f4e5331 unit=25.60 px/cell
  body=910,762..1036,838 pin0=1050,800 value="1.0000001"
```

`tests/float-ops-playtest.ps1` 就是用它把截图和"截图那一帧"对上的（相机在首次绘制之后还会移动，
所以只记第一帧是不够的）：取截帧那行之前的**最后一行**，再按这些屏幕坐标量像素。不设这个变量时
只写一条有界的"注册后第一帧"记录，正常游玩不受影响。

每个真机用例都会复制一份游戏到 `build/<name>-<guid>/game`，并把 `USERPROFILE`/`APPDATA`
指向副本内的 `home`，因此不会碰到玩家存档。用例失败时按日志里的目录路径进去看
`loader.log`、`stdout.txt`、`native-logic-source*.txt`。复现命令见
[../verification.md](../verification.md)。
