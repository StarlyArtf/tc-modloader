# 验证体系与场景清单

所有“已验证”结论都来自三类测试之一，且都在**隔离副本 + 独立存档**中运行：

| 类型 | 位置 | 覆盖 |
|---|---|---|
| 单元／模型测试 | `build.ps1` 内编译并执行 | 游戏对象模型、组件模型、时序图算法、宿主契约（能力表与版本约束） |
| 脚本集成测试 | `node tests/run.js`、`tests/native.ps1`、`tests/saves.ps1`、`tests/setup.ps1` | 包部署/恢复/补丁、原生 Hook、存档隔离、安装器 |
| 真机沙箱用例 | `tests/*-playtest.ps1` | 元件导入/放置/保存、声明延迟、原生逻辑回调 |

`tests/test-catalog.json` 是自动化清单的单一入口：测试 ID、层级、执行器、超时、是否需要
指定游戏构建都写在这里。`tools/test.ps1` 校验清单后串行执行，按用例保存 stdout/stderr，
并在 `build/test-results/` 生成 `results.json` 与 JUnit `results.xml`。人工用例带
`manual: true`，即使 `-Tier all` 也不会意外弹出可见游戏窗口。

## 怎么跑

```powershell
# 不需要游戏二进制：完整构建、编译型单测、包管理与存档模型
./tools/test.ps1 -Tier fast

# 需要指定构建的文件、但不启动真实游戏 UI：原生宿主、钩子链与安装器
./tools/test.ps1 -Tier host

# 真机沙箱回归；自动先构建，已有新鲜构建产物时可加 -NoBuild
./tools/test.ps1 -Tier game

# 所有非人工用例
./tools/test.ps1 -Tier all

# 发现／定点运行
./tools/test.ps1 -List
./tools/test.ps1 -Name native-logic -NoBuild
./tools/test.ps1 -Name 'ui-board-panel*' -NoBuild
```

默认遇到第一项失败就停止；调查多个独立失败时加 `-KeepGoing`。真机用例仍可直接运行原脚本，
例如 `./tests/custom-or-playtest.ps1 -Scenario shape_adder8`。目录中的 probe→driver 顺序就是
全量执行顺序，因此板侧栏和键盘用例会先记录真实矩形，再发输入。

## 公共沙箱模块

`tests/lib/Sandbox.psm1` 统一负责可复用的游戏副本、Mod 部署、独立用户目录、进程收尾和
等待结果文件。它会在复制前检查游戏文件与构建产物，并拒绝删除 `build/` 之外的目录。
`tests/make-ui-sandbox.ps1` 已改用该模块，所以页面、绘图、纹理、键盘、板侧栏和波形等
共用这个准备入口的测试不再各自维护复制规则。其他历史用例保持原行为，后续修改到它们时
再逐步迁移，避免一次性改写已经验证过的真机断言。

## 显示、窗口尺寸与 DPI（2026-09-18）

## 宿主契约：能力协商与依赖版本（0.6.0）

对应实现见 [reference/capabilities.md](reference/capabilities.md)；这里只记"凭什么算过"。

**离线**（`build.ps1` 内执行，`tests/capabilities.cpp`）：

| 断言 | 说明 |
|---|---|
| 能力表自洽 | 每个位都能按名字找回（`capability_bit(name)==bit`）、没有重复位、`capability_names()` 覆盖全部位、名字全小写无空格 |
| 版本只有一处 | 读仓库 `VERSION` 与 `TC_MODLOADER_VERSION_STRING` 逐字比较，并核对 `TC_MODLOADER_VERSION_CODE` 与 `TC_HOST_VERSION_CODE` 一致 |
| 版本序 | `1.10.0 > 1.9.0`（字符串比较会弄反）、`1.0 == 1.0.0`、`v2.0.0 == 2.0.0`、`1.0.0-beta == 1.0.0`、纯文本回退字符串比较 |
| 约束语义 | `*`/空/相等/不等/四个比较/逗号联合；`>=` 缺操作数、`1.0.0` 对空版本等一律**拒绝**而不是放行 |
| 依赖两种写法 | 数组与对象写法都解析；重复 id 只留第一次；非列表、非法 id、非字符串约束分别报错；缺键不是错误 |
| 能力声明的四种拒绝 | 未知名、加载器不提供、非数组、非字符串条目；未知名与"不提供"是两条不同消息 |
| 文档一致 | `docs/reference/capabilities.md` 必须出现表里每个 `` `name` ``，防止文档与代码漂移 |

**脚本**（`node tests/run.js`，共 35 项，其中 11 项本轮新增）：CLI 的
`--version` 与 `VERSION` 一致、`--capabilities` 名单、能力声明被接受、未知名在扫描期被拒
（`list` 显示、`apply` 拒绝）、约束满足/不满足（且**磁盘无改动**）/数字序/可选依赖缺失可
接受/可选依赖一旦启用仍检查/约束写成数字被拒。

**真机**（`tests/ui-board-panel-playtest.ps1`）：`example.board-panel` 现在加载期先
`tc::hostHas(h, TC_CAP_UI_SLOT)` 再注册，并调用 `tc::reportStatus`。日志证据（沙箱
`loader.log`）：

```text
TC Mod Loader 0.6.0 capabilities: log, symbol, hook, logic, component, ui_page, ui_slot, texture, status
[dev.board-panel-driver] status: Board panel: registered slot 'main'
Native loaded: dev.board-panel-driver; hooks=7
```

第二行同时证明两件事：插件读到了 `capabilities`（否则它按新代码直接返回 2、面板不会注册，
测试也不会通过），并且 `report_status` 的结果**没有被**加载器随后的"运行中"覆盖。

**清单全量对照**：把 `dist/*.mod` 全部放进一个临时目录跑
`tcmod-cli <dir> list`，29 个包（含所有示例与开发探针）都没有能力相关错误——
每个示例的 `capabilities` 声明与它实际调用的入口一致。

## 符号画像与钩子链（0.6.0）

**离线**（`build.ps1` 编译 `tests/hook-chain.cpp`，`tests/hook-chain.ps1` 驱动）：
测试宿主自己定义与真机同名的 `sim_do` / `sim_get_cycle` / `load_level` 假符号，
探针包（`tests/hook-chain-probe.cpp`）加入链并把每一步通过 `tc_test_note` 报回宿主。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/hook-chain.ps1
```

| 场景 | 断言 |
|---|---|
| 顺序与参数 | 优先级 0 的 `a0`、同优先级但 Mod id 更大的 `b0`、优先级 10 的 `a10` 依此运行；`a0` 看到 1000，`a10` 看到 1002（前一个链节的修改传下去了），游戏自身函数拿到 1003 且只调用一次 |
| 吞掉原函数 | 链节置 `skip_original` 后，后面的链节不再运行，游戏自身函数一次都没被调用 |
| 链点保留 | 普通目标（`sim.cycle`）原始 Hook 成功；对 `sim.do` 的原始 Hook 被拒 |

日志证据（探针包与加载器各写一部分）：

```text
Hook chain sim.do joined (priority 0)                       <- 插件侧
Hook chain sim.do installed with 3 link(s): dev.hook-chain-a@0, dev.hook-chain-b@0, dev.hook-chain-a@10
PASS hook chain: three links ran in priority/mods order, edits reached the game function, and it still ran once
```

**脚本**（`tests/native.ps1`）：冲突场景改成两个包抢同一个普通目标——第一个
`raw plain-hook ok=1`，第二个 `Hook rejected: phase, target or conflict` 且 `ok=0`；
同一脚本的正常/篡改/禁用三种场景继续用 `example.cycle-guard` 走链拦截
（`100000 -> 200`）。

**真机**（两个沙箱，各自独立存档）：

| 用例 | 证据 |
|---|---|
| `tests/waveform-playtest.ps1` | `Symbol profile: 20/20 aliases resolved`；`Hook chain level.load installed with 1 link(s): dev.waveform-demo-driver@0`；波形、VCD、导线探针断言全过 |
| `TC_SIM_STATE_EXTRA_MODS=example.cycle-guard` + `tests/sim-state-playtest.ps1` | 同一个 `sim.do` 点上有两个链节：`Hook chain sim.do installed with 2 link(s): dev.sim-state@0, example.cycle-guard@0`，关卡跑完、状态映射断言全过（`input_replay slot 0/8`、`output_history slot 55/64`） |

`Symbol profile: 20/20` 是符号画像的正面证据：表里**每一个**别名都在真机构建里解析成功；
解析失败的项会逐条写成 `Symbol profile: unresolved <别名> (<COFF 名>)`
（离线测试宿主只定义 4 个假符号，日志里就能看到另外 16 条，说明缺失不是静默的）。

## 事件总线（0.6.0）

**离线**（`tests/hook-chain.ps1` 的 `events` 模式）：测试宿主额外提供与真机同名的
`change_scene` / `save_level_data` / `save_count` 假符号，探针包订阅全部四种事件。
断言：四种事件都到达同一个监听器、顺序与调用顺序一致、负载正确
（`event sim.command command=0`、`event level.load subject=0`、`event scene.change`、
`event save count=3`），并且仿真命令仍然原样送到游戏自身函数（监听器不改变行为）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/hook-chain.ps1
```

加载器侧同时断言四个事件源都被装载（否则事件不可能到达）：

```text
Event source level.load: ok
Event source sim.do: ok
Event source scene.change armed (Board handle tracking)
Event source save armed
PASS event bus: sim command, level load, scene change and save reached one listener, in order, without disturbing the game
```

**真机**（`tests/waveform-playtest.ps1`）：`example.waveform-demo` 现在订阅关卡加载事件拿板模型
（不再自己进 `level.load` 链），波形、VCD 与导线探针断言全过；日志：

```text
[dev.waveform-demo-driver] Waveform: board model capture subscribed to the level-load event
Event source level.load: ok
Hook chain level.load installed with 1 link(s): @-2147483647     <- 加载器自己的链节
```

## 游戏对象句柄（0.6.0）

**离线**（`build.ps1` → `build/game-handles-test.exe`）：签发、解析、kind 拒绝、代次失效、
棋盘场景进入与离开。关键一条是真机探针逼出来的：关卡加载**同一帧**的场景切换属于“进入”，
必须保留句柄；之后的切换才算离开。

**真机**（`tests/game-handle-probe-playtest.ps1`，游戏层用例 `game-handle-probe`）：
`dev.game-handle-probe-driver` 按主页自带的关卡入口进入关卡、按 **Escape**（玩家自己的离开
动作，不是驱动造出来的切换）让游戏自己调用 `change_scene(ctx,0)`，然后再进一次、再退一次；
插件只通过公开 ABI 报告。一次真实运行：

```text
PROBE: Board unavailable on the main menu (unavailable)
DRIVER: pressing the level entry (rva=0x44a308) to reach the first level
PROBE: level.load name=(none) subject=0xea8bbe2170 frame=282
PROBE: level handle generation=2 token=1 size=24 resolve=ok resolve-match=1 valid=1
Game handles: scene 1 in the level's own frame; the Board handle stays valid
PROBE: Board available generation=3 token=2
DRIVER: trying the game's own way out (Escape)
PROBE: scene.change scene=0 frame=473 old-handle-valid=0
PROBE: old handle resolve=stale pointer=cleared
PROBE: current after scene change=unavailable
DRIVER: self-exit=ok
DRIVER: the game left the level by itself and the old handle was invalidated
DRIVER: pressing the level entry (rva=0x44a308) to reach the second level
PROBE: previous handle valid=0 after the new level load
PROBE: level handle generation=5 token=3 size=24 resolve=ok resolve-match=1 valid=1
DRIVER: trying the game's own way out (Escape)
PROBE: scene.change scene=0 frame=1076 old-handle-valid=0
DRIVER: self-exit=ok
DRIVER: the game left the second level by itself and its handle was invalidated
DRIVER: done
PASS Board handles: ...
```

这个用例是**唯一**发现“进入关卡的场景切换把句柄当场作废”的地方：离线注册表测试没有帧概念，
假宿主也不会真的切场景，所以只有真机生命周期能暴露它。同一次运行还确认了
`scene.change` 在加载插件之前就被加载器自持（`Event source scene.change armed (Board handle
tracking)`），插件侧的 `create_hook` 会被拒绝并提示改用事件。

`self-exit=ok` 是硬断言：如果某个构建里 Escape 不再离开关卡，脚本会失败并提示去看日志，而不是
静默走驱动自己的 `change_scene` 回退分支——回退路径只保证“加载器这一段是对的”，不能证明
“玩家的动作会走到这里”。

两次进入用的是**同一个关卡入口**，原因写在这里免得下次再花一轮去查：用
`TC_HANDLE_PROBE_TRACE=1` 逐条试过主页的五个关卡方格，#3、#4 按下后只是打开该关卡自己的界面
（那里测到的按钮是 `Reset` 和一个空标签按钮，再按后者也不会 `level.load`），#5 会让**游戏自己
以退出码 0 结束**，只有 #2 真正加载关卡——也就是说这是干净存档的解锁状态，不是探针的能力问题。
所以探针验证的是“玩家再次进入关卡”这条生命周期事实（新代次、新 token、旧句柄被拒），而
“进入另一张关卡地图”需要一份已解锁的存档；驱动里的入口是按调用点 RVA 精确按下的，换成别的
存档同样成立。

`tests/ui-board-panel-playtest.ps1` 的“切场景后棋盘停止绘制”步骤同时恢复：驱动改从
`TC_EVENT_SCENE_CHANGE` 的 `subject`（即游戏自己传给 `change_scene` 的 context）取值，
`DRIVER: board stopped drawing after the scene change; panel frames=849` 重新出现，脚本里
对应的 NOTE 分支已改回硬断言。

## 仍未做到：逐周期回调

目标：仿真跑得比渲染快时（"连续运行"）每个周期都能被采样，而不是跨周期丢点。

查证（本次，结论是"不能靠挂钩子做到"）：

```powershell
nm "Turing Complete.exe" | Select-String "step|cycle|tick"        # 只有 UI 的 next_cycle / 设置项，没有每周期步进函数
nm "Turing Complete.exe" | Select-String "compile95thread"        # 只有 sim_do / sim_get_cycle / sim_stop_and_refresh / compile_thread*
nm "compile.dll"        | Select-String "step|cycle|tick|run"     # 只有 GC/注册表相关的 cycle
```

`sim_do` 是"请求运行到第 N 周期"，真正的循环在 `compile_asm` / `compile_isa` 生成的机器码里
（这也是原生逻辑回调只能在生成源码里插入调用、而不是挂在某个函数上的原因）。
EXE 里没有可以钩的步进函数，因此**逐周期回调需要改走生成源码注入**——与
[sdk/custom-logic.md](sdk/custom-logic.md) 的桥接同一类工作，另立一项。

## 安全模式与崩溃隔离

目标（地基第四项的一半）：插件回调里的非法访问不应该带走整个游戏。

**结论：没有做到，只做到"崩溃署名"**。过程与实测如下，免得下次重新踩一遍。

1. MinGW-w64 的 GCC **没有** `__try`/`__except`（实测：`'__try' was not declared in this
   scope`），所以第一版用 VEH + `setjmp/longjmp`。
2. 该机制在孤立环境里**可行**（`tests/fault-guard-probe.cpp` + `tests/fault-guard-helper.cpp`，
   `build.ps1` 内执行，四种情形全部恢复并继续运行）：
   同模块内故障、经 MinHook detour 的故障、在所加载 DLL 内故障、以及"经 MinHook patch
   的 EXE 函数调用 DLL 内故障"的组合（最后一项正是钩子链的调用形态）。
3. 但在加载器里**不可行**：把守卫接到链节回调上，处理函数确实执行（`fault.log` 写下了
   `access violation at 0x…`），进程随后仍然以 **`0xC0000428` /
   `STATUS_INVALID_IMAGE_HASH`** 结束。为排除"插件 DLL 的锅"，又做了一次对照——把故障
   点放进**加载器自己的受保护代码块**（`TC_MODLOADER_FAULT_TEST` 临时探针），结果完全相同。
4. 因为"一半时候管用"的安全网比没有更危险，最终交付的是**可依赖的那一半**：
   `src/fault_guard.hpp` 只登记归属（哪个 Mod、哪个回调/钩子点、什么异常、访问地址），
   然后放行异常让进程照常崩溃。真机/离线验证见下。

要真正隔离，需要换一条路（本次未做）：用 MSVC 编译一个小 shim 提供 `__try/__except`
（项目当前只有 MinGW 工具链），或者在生成代码的边界上捕获（原生逻辑桥已经会往生成源码里
插调用，那里是唯一能拿到"每周期/每次外调"结构的地方）。

**验证**（`tests/hook-chain.ps1` 的 `crash` 模式）：探针包 `dev.hook-chain-crash` 的链节
故意写空指针；脚本要求进程**非 0 退出**（和没有加载器时一样），并且
`tc-modloader-data/fault.log` 同时包含 Mod id、钩子点与异常原因：

```text
dev.hook-chain-crash in sim.do: access violation at 0x0000000000000008
```

`src/loader.cpp` 的日志轮转（8 MiB，保留 `loader.log.1`）在 `tests/native.ps1` 与各真机
用例的日志检查中一并覆盖（日志行格式未变，所以已有的断言不受影响）。

加载器现在每次启动记一行显示诊断（`Display: …`），因为这台机器本身就跑在缩放桌面上：

```
Display: dpi-awareness=per-monitor-aware window-dpi=168 monitors=1
         screen=2560x1600 client=2560x1600 surface=2560x1600 ratio=1.000000,1.000000
```

`window-dpi=168` 就是 175% 缩放，进程是 per-monitor-aware（EXE 里没有 DPI 清单，说明是
运行时设置的）。也就是说所有点击/几何断言**本来就是在 175% 缩放的桌面上跑通的**；
`ratio` 是 ImGui 画布尺寸与 OS 客户区尺寸的比，正是测试把 ImGui 坐标换算成点击坐标时用的
那个系数，写在日志里便于以后对照。

窗口尺寸变化是自动化能覆盖的那一半，`tests/ui-board-panel-playtest.ps1` 新增一段：

1. 面板已经打开、滚动条也拖过之后，驱动把游戏窗口从 `2560x1600` 改成 `1792x1120`；
2. 断言加载器日志里出现按新窗口尺寸排布的面板矩形（`… window=1792x1120`）；
3. 断言插件在重排后上报的按钮绝对坐标**与之前不同**（`panel click sent 2152,334` →
   `panel click after resize sent 1384,261`），并且真实点击仍然命中
   （`Board panel: Ping clicked, count=2`）。

同一轮还顺手修掉一处布局浪费：内容子区域不再保留引擎自己那条（在本构建里不可交互的）
滚动条，插件拿到完整宽度（内容宽从 516 变为 530），宿主绘制的滚动条仍是唯一的滚动入口。

剩下只有一件需要人：**把窗口拖到另一台不同缩放的显示器上**再点一次。本机只有一台
显示器（`monitors=1`），脚本无法制造这个场景；`Display:` 那一行会在换显示器后给出新的
`window-dpi` 与 `ratio`，配合 `tests/ui-board-panel-playtest.ps1` 的点击断言就能判断
命中是否仍然正确。

## 中文输入与板上打字：人工验收（2026-09-18，用户确认）

`tests/manual-ime-test.ps1` 准备的一键沙箱（`build/manual-sandbox`，独立
`USERPROFILE/APPDATA`）里由用户实际操作完成，结论通过，并留下可核对的数据：

| 检查项 | 结果 |
|---|---|
| 页面里的中文输入（组合串、选词上屏、Esc 取消、中文标点） | 通过 |
| 电路板侧栏面板里用输入法打字 | 通过：缓冲区出现 `board panel buffer "asdasa啊啊倒莓的啊…"`（UTF-8） |
| 候选窗／组合窗定位 | **跟随光标**：`IME board panel composition=1970,259 candidate=1970,259 style=0x32`，随每个字依次 1990→2011→2032→2053→2073→2094→2250→2281→2301→2322，删字时退回 2301；`style=0x32 = CFS_POINT\|CFS_FORCE_POSITION` |
| 板上打字时的快捷键穿透 | 未发生：面板收到 `board panel saw key A`、`board panel saw key Space`，而画面里没有元件放置、仿真运行等板面反应（用户确认） |
| 观感（裁剪、滚动条、是否挡住游戏 UI） | 通过 |
| 已知现象 | 独占全屏下输入法候选窗出现时画面会**闪一下**（用户观察，见下方说明） |

**关于全屏闪烁**：候选窗是 Windows 自己画的独立窗口，不由加载器或插件绘制。独占全屏时
系统需要把它合成到游戏画面之上，OpenGL 交换链在这种切换中闪一帧是常见现象，加载器无法
从这一侧消除。可选做法（按代价从低到高）：改用无边框窗口化全屏；在输入法设置里关闭
候选窗的内嵌/动画选项；在 exe 兼容性里关掉"全屏优化"再试；若想知道到底是系统合成还是
游戏自己在切换窗口模式，可以在探针里加一段记录窗口样式/位置变化的诊断，复现一次即可判定。

## 键盘与字符输入验证（2026-09-18）

`tests/ui-keyboard-playtest.ps1`（先 `-Probe` 记录页面入口矩形，再跑驱动）在
`build/keyboard-sandbox` 里打开一个带 InputText 的插件页面，驱动
`tests/ui-keyboard-driver.hpp` 发**真实窗口消息**：带扫描码的 `WM_KEYDOWN/UP`、
`WM_CHAR`、`WM_IME_CHAR`。探针 `tests/ui-keyboard-probe.cpp` 把收到的键、焦点变化和
缓冲区内容写进日志，断言只看日志：

| 断言 | 证据 |
|---|---|
| 按键到达插件控件 | `Keyboard probe: key Tab pressed/down/up`、`key Escape pressed`、`key A pressed` |
| 真实按键打字不重不漏 | 只发按键消息（真实键盘的等价路径）：`buffer "" → "abc" length=3` |
| 单独的字符消息同样不重不漏 | 只发一条 `WM_CHAR`（z）：`buffer "abc\xe4\xbd\xa0\xe5\xa5\xbdz" length=10` |
| 非 ASCII 码元到达 | `buffer "abc\xe4\xbd\xa0\xe5\xa5\xbd" length=9`，即 `你`（WM_CHAR）与 `好`（WM_IME_CHAR）都进了缓冲区 |
| Escape 被输入框吃掉 | 之后是 `buffer ""`（ImGui 回滚文本），没有 `Closed UI page` |

同一条测试顺手纠正了一个**误判**（最早写在这份文档里的"字符会被重复投递"）：

| 发送内容 | 缓冲区结果 |
|---|---|
| 只发 `WM_KEYDOWN/UP`（真实键盘路径） | 1 个字符 |
| 只发 `WM_CHAR` | 1 个字符 |
| `WM_KEYDOWN/UP` + `WM_CHAR`（第一版驱动的错） | 2 个字符 |

原因是按键消息会被消息循环的 `TranslateMessage` 转成 `WM_CHAR`（与任何 Windows 程序
相同），再手工补一条就是两个字符。真实打字只有一条 `WM_CHAR`，因此**不需要插件去重**，
[sdk/ui.md](sdk/ui.md) 的能力表也按此更正。中文**组合串与候选窗**（preedit、
ImmSetCandidateWindow 定位）无法脚本化，留在同一份文档的人工验收清单里。
离线部分：`tests/ui-key.cpp` 检查 `keys::*` 的严格解析（缺导出即失败并报名字）、
未加载时是安全空操作、参数逐个转发，以及枚举值与实测一致。

## 电路板侧栏插槽验证（2026-09-18）

`tests/ui-board-panel-playtest.ps1` 在 `build/board-panel-sandbox` 独立副本里驱动真实
游戏（模式：`-Probe` 记录几何、默认驱动模式发真实点击、`-Example` 只验证发行示例）。
驱动代码 `tests/ui-board-panel-driver.hpp` 编进测试包 `dev.board-panel-driver`
（`build.ps1` 生成），发行示例 `example.board-panel` 不含测试代码。

这条测试一次跑完几件事，全部来自日志而不是推断：

| 断言 | 证据 |
|---|---|
| 注册与绘制 | `UI slot registered: main (board side panel)`、`Board panel dev.board-panel-driver/main frame=… x=1955 y=153 w=576 h=480 window=2560x1600` |
| 只在关卡里出现 | 面板第一帧日志出现在 `DRIVER: pressing a home page entry to reach a board` 之后；主菜单帧没有面板日志 |
| 真实点击到达插件 | `DRIVER: panel button hovered on frame …`、`DRIVER: panel button click reported count=1`、`Board panel: Ping clicked, count=1`；点击由 `WM_LBUTTONDOWN/UP` 加真实光标位置发出，目标点由面板矩形与按钮自身报告的位置算出 |
| 点击没有穿透到电路板 | 游戏自己的采样：面板点击窗口 `gate[panel] … active=1 bg=0 => busy=1`；同一测试在画布点做正对照 `gate[canvas] … active=0 => busy=0`，证明闸门不是恒为忙 |
| 裁剪 | 内容区下方 160px 处画一个探针控件，并按它"本应出现"的屏幕坐标发真实点击：拖动前 `clipped probe before scrolling hovers=0 clicks=0`，即被裁掉的项目既不可悬停也不可点击 |
| 面板边界 | 同一次点击落在面板矩形之外，游戏采样为 `busy=0` —— 面板只声明自己的矩形，面板外的点击仍归电路板 |
| 滚动 | 真实鼠标拖动宿主绘制的滚动条：`Host scroll strip … active=1` → 加载器直接读取的内容区滚动量 `Host content scroll … y=73 max=1268` |
| 折叠／展开 | 点标题栏开关：加载器记录 `h=60 … collapsed=1`（折叠帧仍显示旧矩形，所以断言的是一条标题栏高的新矩形）；折叠期间点按钮**不计数**（`count` 停在 1），展开后再点得到 `count=2`；折叠期间插件仍在画（`Board panel: draw count=…`） |
| 随场景关闭 | 驱动调用游戏自己的 `change_scene(ctx,0)`（ctx 来自加载器的 `TC_EVENT_SCENE_CHANGE` 负载，即游戏自己传给 `change_scene` 的那个对象），随后 `DRIVER: board stopped drawing after the scene change; panel frames=849`，之后不再有面板帧。这是硬断言：`scene.change` 改为加载器自持后这条曾退化成 NOTE，事件补上 `subject` 后恢复 |

实测日志（一次通过运行，节选）：

```
[dev.board-panel-driver] UI slot registered: main (board side panel)
[dev.board-panel-driver] DRIVER: panel first drawn on frame 122, content 550x412
Board input sample site reached; board panels are drawn from here
[dev.board-panel-driver] DRIVER: panel click sent 2147,335 tag=panel
[dev.board-panel-driver] DRIVER: gate[panel] frame=329 active=1 bg=0 hovered=0 => busy=1
[dev.board-panel-driver] DRIVER: panel button click reported count=1
[dev.board-panel-driver] DRIVER: canvas click sent 1280,700 tag=canvas
[dev.board-panel-driver] DRIVER: gate[canvas] frame=409 active=0 bg=-1 hovered=1 => busy=0
[dev.board-panel-driver] DRIVER: scrollbar drag sent 2488,231 +220 tag=scrollbar
Board panel scroll strip dev.board-panel-driver/main hovered=1 active=1 max=1268 scroll=0 x=2483 y=207 w=19 h=412
Board panel content scroll dev.board-panel-driver/main y=73 max=1268 contentEnd=489 childH=412
[dev.board-panel-driver] DRIVER: collapse toggle clicked at 2501,183 tag=collapse
Board panel dev.board-panel-driver/main frame=724 x=1955 y=153 w=576 h=60 … collapsed=1
[dev.board-panel-driver] DRIVER: button click while folded sent 2140,334 tag=folded (must not register)
[dev.board-panel-driver] DRIVER: expand toggle clicked at 2501,183 tag=expand
[dev.board-panel-driver] Board panel: Ping clicked, count=2
[dev.board-panel-driver] DRIVER: board stopped drawing after the scene change; panel frames=173
PASS board side panel: registered, drawn 173 frames on the board only, real click delivered …
```

离线部分：`build.ps1` 里的 `tests/ui-slot.cpp` 检查 `registerBoardPanel` 的宿主版本检查
（旧宿主按结构大小返回 −1，不越界读）、参数校验，以及交给宿主的定义内容。

两条测试基建教训记在这里，避免下次再踩：**沙箱必须每次刷新加载器**
（`make-ui-sandbox.ps1` 负责把 `dist/tc-loader.dll` 复制成 `game_engine.dll`，旧沙箱会
静默测到上一版加载器）；以及 **MinHook 钩子会跟进 `jmp` thunk**，此时被钩函数看到的
返回地址不再是被钩入口的调用点——加载器因此在直达路径之外增加了一次短线栈回溯。

第三件：**本构建的鼠标滚轮到不了 ImGui**。帖子窗口的 `WM_MOUSEWHEEL`（PostMessage 与
SendMessage 都试过）既没有改变内容区的滚动量，也没有改变其它可观测状态；内容子窗口本身
是可滚动的（`SetScrollY(90)` 读回 90、`ScrollMax=1268`），所以这不是子窗口的问题。因此
面板的滚动条由宿主自己绘制并驱动（拖动即滚动），滚轮相关的能力不做承诺。

## 波形面板：真机数据回放（2026-09-18）

`tests/waveform-playtest.ps1` 在 `build/waveform-sandbox` 独立副本里跑
`example.waveform-demo` 的驱动构建 `dev.waveform-demo-driver`（`build.ps1` 生成）。
驱动器 `tests/waveform-driver.hpp` 自己进入关卡：按主页入口的隐形按钮进入电路板、
从 `handle_update_wire` 拿 model、加载 `and_gate`、请求编译、把仿真跑到 cycle 6；
沙箱里预置了内置 AND 解（`build/and2_solution_builtin.data`）作为被测试的电路。
交接文档（现状、实测数据、未完成项与下一步做法）见
[research/waveform-handoff.md](research/waveform-handoff.md)。

断言全部来自日志与文件，不靠"看起来对"：

| 断言 | 证据 |
|---|---|
| 面板注册与绘制 | `UI slot registered: waveform (board side panel)`、`Waveform: registered board panel`、`Waveform: panel first drawn on frame …`；示例包 `hooks=0`，面板由加载器调度 |
| 槽位解析 | `Waveform: resolved slots in0@0 in1@8 (moved 2) out0@55 out1@64 (moved 2) (all found by movement)` —— 与离线单测里的实测布局一致；解析结果每变化一次就记一行，所以第一次"还没跑起来"的推断（`moved 0` + `stride estimated`）与之后按字节变化找到的最终结果都能看到 |
| 波形数据＝关卡自带测试 | 驱动器逐行上报面板要画的内容：`DRIVER: row … in=0,0 out=0,0` → `in=2,2 out=0,0` → `in=3,3 out=1,1`；脚本断言输出为高的行必定输入为 3、输入从 0 起、最大值 3、最后一行是 in=3/out=1 |
| 导出 VCD | `DRIVER: export rows=N path=…/waveform.vcd`；脚本读文件断言 4 个 `$var wire 64` 信号、有 `#3` 时间戳、`b11 i0` 与 `b1 o0` 都在 |
| 只在仿真时更新 | 驱动器记录行数 → 让游戏**暂停 2 秒** → 再记录：`DRIVER: paused rows=1` 与 `paused rows after 2s=1` 必须相等（暂停期间一行都不加）；开始运行后逐行断言 `cycle` **严格递增**，即一行对应一个新周期，而不是一帧一行 |
| 引脚数量与"重新运行清空" | 离线（`tests/trace.cpp`）：`setDeclaredCounts(1,1)` 后 `inputCount()/outputCount()` 变成 1/1；周期从 5 退回 −1 时行数清零、`takeRestartFlag()` 为真、随后从新的一轮重新记录。真机日志：`Waveform: the board has 2 input(s) and 2 output(s)`（板上 IO 元件数，替代不可信的 `level_used_input/outputs` 全局量） |
| 波形确实画在屏幕上 | 面板自己用 `glReadPixels` 抓帧写 `waveform.bmp`，脚本在**加载器报出的面板矩形**（`Board panel dev.waveform-demo-driver/waveform … x=1763 y=153 w=768 h=672`）内逐像素统计泳道颜色：一次通过运行得到输入蓝 `(90,170,235)`、输出黄 `(245,185,70)` 各数十像素（一个周期一行时波形很短，阈值只用来证明两条 lane 真的画出来了） |

实测日志（一次通过运行，节选）：

```
[dev.waveform-demo-driver] Waveform: resolved slots in0@0 in1@8 (moved 0) out0@0 out1@8 (moved 0) (stride estimated)
[dev.waveform-demo-driver] Waveform: resolved slots in0@0 in1@8 (moved 2) out0@55 out1@64 (moved 2) (all found by movement)
[dev.waveform-demo-driver] DRIVER: row 3 cycle=0 in=0,0 out=0,0
[dev.waveform-demo-driver] DRIVER: row 4 cycle=2 in=2,2 out=0,0
[dev.waveform-demo-driver] DRIVER: row 5 cycle=3 in=3,3 out=1,1
[dev.waveform-demo-driver] DRIVER: traced 4 rows, inputs=2 outputs=2, last row cycle=3 in=3,3 out=1,1
[dev.waveform-demo-driver] DRIVER: export rows=4 path=…\waveform.vcd
[dev.waveform-demo-driver] Waveform: exported …\waveform.bmp (framebuffer)
[dev.waveform-demo-driver] DRIVER: image …\waveform.bmp
PASS waveform panel: … added no rows during a two-second pause and one row per new cycle …
```

**这台机器上抓不到游戏窗口的像素**（重要，会影响后续所有"截图"类工作）：本构建全屏
运行在**独占翻转**的 GL 窗口上，`PrintWindow(hwnd, dc, 0)` 与 `PW_RENDERFULLCONTENT`
都返回整幅纯黑，`CopyFromScreen` 抓到的桌面里根本没有游戏窗口（窗口 `visible=True`、
`iconic=False`、`cloaked=0`，`GetForegroundWindow`/`SetWindowPos(HWND_TOPMOST)` 都不改变
这一点）。仓库里既有 UI 用例写出的 `menu.png`/`driver-page-open.png` 因此也是纯黑——
它们只能当"跑过"的凭证，不能当画面证据。要画面，只能由进程内自己 `glReadPixels`。

离线部分：`build.ps1` 编译并执行 `tests/trace.cpp`，按实测布局验证采样器的槽位发现、
数值读取、稀疏回退与 VCD 文本（`PASS simulation trace: slot discovery, values, sparse
fallback, VCD export`）。

### 内部节点（导线）探针：已实现并真机验证

面板可以监测**电路内部任意一条导线**：在板上选中导线 → **Probe wire**。链路是：

- 板模型：元件序列在 `model+0x78`（步长 `0x238`）、导线序列在 `model+0x98`（步长
  `0x68`）；`get_wire(model, point)` 返回的 id **就是导线在序列里的下标**（实测
  `id(a)=0,1,2` 与下标一一对应）。模型由 `load_level__modelZutilities_u7740` 的第一个参数
  捕获（这个 Hook 目前没有别的 Mod 使用；抢 `handle_update_wire` 会与 wire-palette 冲突）。
- 导线记录 `+0x38` 是它在仿真状态缓冲里的**字节偏移**、`+0x30` 是位宽；值等于
  `sim_state_read_u64(offset) & ((1<<width)-1)`，与游戏自己的 `sim_state_read_bits` 一致
  （反汇编核对：`read_u64` = `*(uint64*)(state_base + offset)`，`read_bits` = 它取低 width 位）。
- **采样时机**：仿真状态在步进结束时才写好。真机对照：暂停在 cycle 2 后读与运行中"周期刚
  变化"时读，同一个字节前者 0/1 正确、后者滞后一个周期。因此 `sample()` 把行延后一次调用
  落盘：捕获时读关卡 I/O，落盘时读探针。

真机断言（`tests/waveform-playtest.ps1`）：驱动器挑"接在关卡输出侧的那条导线"（端点 x 最大），
请面板加探针，然后逐行比较探针值与关卡自身的输出历史——`and_gate` 上两者都是 `0,0,0,1`，
一致；不一致就直接失败。

```
[dev.waveform-demo-driver] Waveform: driver probes wire 2 state@260 width=1
[dev.waveform-demo-driver] DRIVER: probe row=0 cycle=-1 value=0 out0=0
[dev.waveform-demo-driver] DRIVER: probe row=3 cycle=3 value=1 out0=1
Wire probe: driver probes wire 2 state@260 width=1 - value matched the level's own output on all 4 rows
```

未做：字宽网络的位宽字段核对（只用 1 位网络验证过）、元件引脚探针、编辑电路后探针失效的
重新解析。

## 自定义绘图回归（2026-09-18）

`build.ps1` 包含 `tests/ui-draw.cpp`：可选绘图能力缺失时的原子失败、重新加载失败后
清空旧绑定、局部坐标转换、输入状态快照、非法输入不提交、嵌套与异常退出裁剪配对。

`tests/ui-draw-playtest.ps1` 在 `build/draw-test-sandbox` 独立游戏／存档中加载真实引擎，
连续 180 帧验证所有绘图方法有顶点输出，矩形顶点位置／颜色与参数一致，窗口移动和
尺寸变化、子窗口滚动后的几何与裁剪仍正确，嵌套及画布裁剪都恢复原值。
测试专用探针只读顶点缓冲；SDK 不依赖该私有结构。

实测：`DRAW PASS frames=1/60/120/180` 全部通过。未把截图效果、真实拖动交互或多 DPI
支持计入这条自动化测试的结论。手工可启用 `example.drawing-demo.mod`，进入主菜单
Custom drawing 或按 F8，验证拖动控制点、松手计数、网格、线宽、重置与关闭重开。

## 沙箱机制

**别把 wire-palette 装进从现用安装复制出来的沙箱**：`make-ui-sandbox.ps1` 复制的是
`D:\p\asset`（里面已经是打过补丁的着色器），再 `apply local.wire-palette` 会**二次打补丁**，
结果 GLSL 编译失败、游戏启动即退出（进程退出码 1，连 `loader.log` 都不产生）。做界面截图
或临时沙箱时，Mod 列表里去掉 `local.wire-palette` 即可；需要它时就先在沙箱里还原
`asset/shader/*.vert`。

要还原就把现用安装 `state.json` 里该文件的 `original` 哈希对应的
`tc-modloader-data/blobs/<哈希>` 复制回沙箱的 `asset/shader/*.vert`，**再**执行 `apply`
（`tests/waveform-playtest.ps1` 之外的手工沙箱都按这个顺序来）。

## 板内界面怎么截图（开发用）

板侧栏面板、导线调色盘、电路只存在于关卡里，而菜单页的绘制只在主页运行，所以截图要用
`igEnd` 里的**定时抓帧** plus 一个只会“进入关卡”的小探针：

```powershell
$env:TC_MODLOADER_SHOT       = 'D:\shot\board.bmp'
$env:TC_MODLOADER_SHOT_DELAY = '14000'      # 毫秒：等 enter-board 进关卡、面板画出来
# 沙箱里 apply dev.enter-board,local.wire-palette 后启动游戏即可
```

`dev.enter-board`（`tests/enter-board.cpp`）只按一次主页入口，不 Hook 别的目标——板驱动
会因为占用 `handle_update_wire` 把调色盘挤掉（见上一条）。日志出现
`Captured frame screenshot` 后把 BMP 转 PNG 即可查看。

## 工具栏插槽（游戏工具栏里的 Mod 工具）

导线调色盘现在注册为**工具栏工具**（`TC_UI_SLOT_BOARD_TOOLBAR`），验证方式同上（沙箱 +
`dev.enter-board` + 定时抓帧）。一次通过运行的证据：

```
[local.wire-palette] UI slot registered: palette (board tool)
Board tool local.wire-palette/palette drawn in the game's tool column (frame 667, child 2560x932 at 0,718)
Board tools in the game's tool column (top to bottom): local.wire-palette/palette
```

截图可见色块按钮落在游戏工具列里、游戏自己的工具按钮正下方（`build/palette-shots/`）。
注入点是游戏最后一个工具按钮的 `igEndChild` 返回地址（`0x4659c1`，从实测的各子窗口几何
里挑出来的：工具按钮都是 x=366/462、y=174…666、80×80 的子窗口），绘制发生在该子窗口**结束
之后**，所以控件落在工具栏自己的窗口里；列的位置由加载器从这些按钮实测得到
（`noteToolColumn`），不是写死坐标。

### 展开界面里的文字（2026-09-18，已解决）

`tests/wire-palette-tool-playtest.ps1` 在 `build/wire-palette-tool-sandbox` 独立副本里跑真实
调色盘：驱动 `tests/toolbar-hover-driver.cpp`（`dev.toolbar-hover`）先进关卡，再把**真实鼠标**
压在插件瓦片上（本构建从真实光标读鼠标位置，所以只能 `SetCursorPos`），定时抓帧得到
`build/wire-palette-tool-out/open2.png`：展开面板里"导线调色盘/取色器/使用此颜色/新建颜色…"
等中英文都正常。这条用例同时打印：

```
Tool column text font: defined_fonts[2]=NoroshiCode_Regular.ttf (used for plugin tool draws)
Board tool local.wire-palette/palette drawn in the game's tool column (frame 346, child 2560x932 at 0,761)
Board tools in the game's tool column (top to bottom): local.wire-palette/palette
```

根因与证据（探针 `tests/toolbar-font-probe.cpp` + `tests/toolbar-font-playtest.ps1`，
反汇编见 `tools/scan-calls.js` 与 [research/toolbar-tool-handoff.md](research/toolbar-tool-handoff.md) §5）：

| 断言 | 证据 |
|---|---|
| 注入点当前字体是**图标字体**，不是字号问题 | `TOOLBARFONT tool ... font=Icon_Complete.ttf size=72`，同一插件在侧栏面板/主菜单页/帧尾都是 `NoroshiCode_Regular.ttf size=45` |
| 图标字体没有拉丁/中文字形，所以文字一个顶点都不产生 | 图标字体下矩形/按钮底可见、文字全无；推入正文字体后同一段文字立刻可见 |
| 游戏自己画工具栏标签的写法 | 反汇编 `build_function_icons__presenterZboard95uiZfunction95icons_u115`：`igPushFont(2)` → `igPushFontScale` → `igText` → `igPopFont()` → `igPopFontScale()`，注入点在这段作用域之外 |
| 宿主借字体后插件无需任何字体代码 | 探针里 `tc::ui::text(...)` 直接显示；调色盘端到端截图同样 |
| 宿主与游戏的字号一致 | 借用正文字体后 `igGetFontSize()` = 45，与侧栏面板相同 |

这条用例的沙箱准备比别处多一步：现用安装的 `asset/shader/*.vert` 已被本 Mod 打过补丁，
直接 `apply` 会二次打补丁、游戏启动即退出，所以脚本先按 `state.json` 里的 `original` 哈希从
`tc-modloader-data/blobs/` 还原这两个文件再 `apply`（顺序见本节「沙箱机制」）。

**关闭路径**同样观测到了：同一次运行里鼠标移开后日志出现
`Wire Palette: the expanded palette closed after the mouse left`，抓帧 `final-open.png` 就是收起
后的画面（瓦片还在、面板消失）。它不是一条"必然通过"的断言——沙箱与使用者共用同一个真实鼠标
与焦点，游戏窗口失焦后 ImGui 的鼠标位置不再更新，几何 hover 会一直为真；需要时可加
`-LeaveAfterMs 2000` 主动把鼠标移开再查日志。

### Mod 页面字号（2026-09-18，修掉一个静默失效的绑定）

页面容器原本按名字从**引擎 DLL** 取游戏的 `igPushFontScale` / `igPopFontScale` 想让标题与主页
同字号，但那两个是**可执行文件**里的 Nim 符号（引擎 1505 个导出里 0 个 `presenterZ…`），
所以一直解析失败、`if (withFont)` 永远为假，标题一直用窗口自带字号——不报错，也没人发现。

现在的做法：只对**宿主自己的页面窗口**调用引擎导出 `igSetWindowFontScale`（页头用主页倍率，
内容区恢复 1.0）。证据来自 `tests/ui-page-playtest.ps1 -DriverMode`（沙箱用
`make-ui-sandbox.ps1 -Mods dev.menu-demo-driver`，先 `-ProbeButtons` 记录入口）：

```
Mod page font scale applied: window font 45 -> 86
[dev.menu-demo-driver] Menu demo: page 'settings' first drawn on frame 124, content 2540x1497
PASS native UI page registered and drawn; screenshots in build/ui-page-sandbox/out
```

反面教训（记下来免得再踩）：把这两个游戏函数从 EXE 符号表绑上、真的去驱动游戏的字体栈，
会让页面**连绘制体都进不去**（`-DriverMode` 报 "The demo page never drew a frame"）。
宿主自己的窗口不需要游戏的栈。

纹理回归 `ui-texture-playtest.ps1` 使用 `build/texture-test-sandbox`。它实际回读 GPU
纹理的 RGBA 像素（包括透明度和上下方向），验证中文路径 PNG、损坏／缺失图片、
越界路径、非渲染线程、解包状态与纹理绑定恢复、UV 顶点、所有权、配额、重复释放、
拒绝初始化资源的延迟回收及连续 180 帧绘制。`ui-texture.cpp` 覆盖旧宿主降级、
移动所有权、重载失败保留原图、析构与显式释放。

每个真机用例都会：

1. 把游戏（EXE、引擎、`asset/`、`campaign/`、`translations/`）复制到 `build/<用例>-<guid>/game`。
2. 把 `USERPROFILE`/`APPDATA` 指向副本内的 `home`，因此存档完全隔离。
3. 用 `dist/tcmod-cli.exe <副本> apply <mod id>` 应用被测包。
4. 需要关卡时，把原理图写到
   `profiles/default/schematics/<关卡>/Default/circuit.data`，并按需写入用例开关
   （例如 `plugin-data/<mod id>/autotest.txt`）。
5. 启动游戏、等待、退出，然后**断言加载器日志、生成源码转储与关卡自身判定**。

失败时脚本会抛出带日志路径的错误；日志解读见 [reference/diagnostics.md](reference/diagnostics.md)。

## 原生逻辑回调场景（11 个）

| 场景 | 元件形状 | 关卡 | 游戏侧证据 | 独立复核 |
|---|---|---|---|---|
| `or` | 2 进 1 出（回调 OR） | `or_gate` | 判定 win，输出脚历史 `0,1,1,1` | 回调每周期恰一次 |
| `mixed` | 2 进 1 出 + 内置 NOT | `nor_gate` | 判定 win，输出脚历史 `1,0,0,0` | 两者都执行才可能通过 |
| `multi` | 同一定义 2 个实例 → 内置 AND | `or_gate` | 判定 win | 两实例各自每周期一次 |
| `shape1` | 1 进 1 出（NOT） | `not_gate` | 判定 win，历史 `1,0` | 输入元组 = 单脚 |
| `shape3` | 3 进 1 出（AND） | `and_gate_3` | 判定 win（8 周期） | 输入元组保序 |
| `shape32` | 3 进 2 出（全加器） | `full_adder` | 判定 win，同时校验 Sum/Carry | 43 样本复核 |
| `shapew8` | 8 位进 / 8 位出（×2） | `double_number` | 判定 win（16 周期随机字节） | UI 表格文本 |
| `shape_xor8` | 2×8 位 → 8 位 | `byte_xor` | 40 周期无失配 | 43 样本满足 `a ^ b` |
| `shape_mux8` | 3×8 位 → 8 位 | `byte_mux` | 40 周期无失配 | 43 样本满足 mux 语义 |
| `shape_asr8` | 8 位 + 3 位 → 8 位 | `byte_asr` | 40 周期无失配 | 43 样本满足算术右移 |
| `shape_adder8` | 1+8+8 → 8+1 位 | `byte_adder` | 关卡校验两个输出脚，40 周期无失配 | 43 样本满足 sum/carry |

另外每个场景都会断言：编译结果里关卡输入/输出元件数量与关卡定义一致、没有关卡输出脚
处于高阻；涉及重置的场景断言所有实例收到 `TC_LOGIC_RESET`。

### 为什么有的场景只断言“无失配”

`byte_*` 系列关卡的胜利条件需要 0xffff／0x1ffff 个周期（几万周期）。对这些场景：

- 自动验证脚本给 `autotest.txt` 写一个周期上限（例如 40）；
- 断言 `run finished cycle=40 ... verdict=0`，即关卡自己在 40 个周期内**没有**判定失败；
- 测试脚本再解析插件日志里回调交换的原始值，独立复核目标函数（43 个样本）。

两个方向都通过，才说明"进入回调的输入、回调算出的输出、写回关卡输出脚"这条链路正确。

## 其他真机用例

| 用例 | 覆盖 |
|---|---|
| `and-component-playtest.ps1` | 电路定义导入：64 位 ID、引脚数量与位宽、名称、快照释放 |
| `component-placement-playtest.ps1` | 组件菜单使用的放置 helper 把 `0x4e` 实例落到 board |
| `component-persistence-playtest.ps1` | 保存电路重启两次后 ID 与导线保持 |
| `component-timing-playtest.ps1` | 声明统计进入编译统计与界面分数（单件/串联/并联/0/内置/嵌套/沙盒/工坊/多驱动，9 例） |
| `byte-adder-smoke.ps1` | `example.byte-adder` 真实包：引脚几何、刷新阶段与周期阶段的加法取样、关卡 40 周期无失配 |
| `native-component-playtest.ps1` | 声明式 8 输入／8 输出：重复输入次序、最后两个输出接关卡、重复 ID／非法描述拒绝、40 周期无失配 |
| `native-component-playtest.ps1 -Single` | 声明式 1 输入／1 输出字节元件：`double_number` 16 周期全部通过 |
| `kind-list-playtest.ps1` | 枚举 125 个内置元件的 kind／名称／引脚（`build/kinds.txt`） |
| `ui-playtest.ps1` | 界面封装：真实 `mod-inspector` 包的面板在 ImGui 帧内绘制，视口/鼠标返回值合理 |
| `waveform-playtest.ps1` | 关卡波形：驱动器进入 `and_gate` 并运行，断言采样行＝关卡自带测试、VCD 内容、以及面板矩形内的泳道颜色像素（`-Example` 只验证发行包注册） |
| `sim-state-probe`（开发探针） | 仿真状态映射与整板解释器实验（非发布路径） |

## 断言来源原则

1. 优先使用游戏自身产生的证据：关卡判定结果、关卡输出脚历史、UI 表格文本、编译统计。
2. 插件日志只用于补充过程事实（调用次数、交换的数值），并尽量在测试脚本里**独立重算**
   目标函数，避免"插件自己说自己对"。
3. 不把开发期探针（`dev.*` 包、`TC_GUARD_SELFTEST` 等）算作玩家功能验证。

## 作者自检清单

- [ ] 包能应用并重启后正常加载，日志无拒绝行。
- [ ] 自定义元件能在 Sandbox/Foundry 放置，关卡测试判定通过（或按本页说明用有界断言）。
- [ ] 元件行为由回调决定（内部电路是占位门），且关卡判定、暂停、重置仍由游戏执行。
- [ ] 多实例场景下每个实例状态独立。
- [ ] 日志中没有 `registration rejected`、`exposes N operand(s)`、`has no recognised internal logic node`。
- [ ] 字宽元件确认输出脚宽度与定义一致，且 16/32 位等未验收组合被明确标注。

## 未验证边界

- 16/32 位字宽关卡（战役里只有 symphony 系列有 32 位 IO，其判定依赖 RAM 与指令执行，不适合作为元件用例）。
- 字宽状态保持（计数器类元件）、暂停语义、3 个以上实例组合。
- 元件内部中间层（按计划暂缓，留待后续独立 Mod）、RAM/寄存器作为逻辑源。
- 实际鼠标拖动、战役通关、不同显卡/窗口配置、多第三方原生插件组合。
- 波形采样只覆盖当前关卡的普通 I/O 历史：字宽关卡不一定写输出历史（这类关卡输出波形
  可能为空），`and_gate` 之外的关卡（引脚更多、输入不连续变化）尚未逐个验收。
- 安全模式已实现但未做物理按键实机验证。
- Steam 云服务联网行为、游戏更新后的重新适配。

# 主菜单页面与游戏外观按钮：验收

对应改动见 [research/native-ui-pages.md](research/native-ui-pages.md) 与
[sdk/ui.md](sdk/ui.md)。下面区分"自动测试证明的"和"需要人看一眼的"。

## 自动测试证明的

```powershell
./build.ps1                                   # 全量构建 + 既有全部单测
./tests/make-ui-sandbox.ps1 -Mods dev.menu-demo,dev.menu-demo-peer
./tests/ui-page-playtest.ps1 -ProbeButtons    # 记录主页按钮矩形
./tests/ui-page-playtest.ps1 -DriverMode      # 打开页面、绘制、输入、容器尺寸
./tests/ui-page-playtest.ps1                  # 两个插件注册同名页面
```

`-DriverMode` 断言的证据（真机运行的真实日志）：

```
DRIVER: container window size 2560x1600
DRIVER: Apply item size 111x51
DRIVER: full-region probe itemHovered=1 windowHovered=1
DRIVER: CONFIRMED - a click reached a widget inside the page container
```

即：容器按视口铺满、控件能收到 hover、点击能到达控件。另外断言两个插件各自注册了同名
页面、页面确实绘制过、`on_frame` 与页面各自计数互不抢帧、没有回调异常。

## 人工验收记录（2026-09-18）

```powershell
./tests/ui-page-demo.ps1 -Peer
```

隔离副本、两个插件包（`dev.menu-demo` 与 `dev.menu-demo-peer`，**相同页面 ID、相同控件
标签**）同时加载，逐项确认：

| 检查项 | 结果 |
|---|---|
| 主菜单出现两个同名页面入口 | 通过 |
| 点入口打开整屏页面（替换主菜单，不是浮层） | 通过 |
| 页面内 `Apply` 可点击、计数递增、悬停变色 | 通过 |
| 返回按钮回到主菜单 | 通过（返回后又打开了另一个插件的页面） |
| 另一个插件的同名页面独立工作、计数互不影响 | 通过 |
| 进关卡后页面不残留 | 通过 |
| 外观观感 | 接受当前状态，**不要求**与游戏原生 UI 完全一致 |

对应日志：

```
[dev.menu-demo] UI page registered: settings
[dev.menu-demo-peer] UI page registered: settings
Opened UI page dev.menu-demo/settings
[dev.menu-demo] Menu demo: page 'settings' first drawn on frame 5017, content 2560x1497
[dev.menu-demo] Menu demo: Apply clicked on settings
Opened UI page dev.menu-demo-peer/settings
```

`Apply clicked` 那一行是人工点击产生的：自动化一直没能稳定复现它，因为合成光标会被
这台机器上其他程序的光标锁定覆盖（细节见
[sdk/ui.md](sdk/ui.md#容器内输入已验证)）。所以真实的鼠标点击这一项由人确认。

## 已知限制（设计如此，非缺陷）

- 页面容器与板侧栏插槽现在共用同一段裁剪／滚动实现（`src/native.hpp`
  `drawContentRegion`），内容超过容器高度时滚动而不是画到区域外；页面容器里的
  滚动行为由插槽用例的真机拖动断言覆盖（同一条代码路径），页面侧只断言内容区
  与滚动条存在（`UI page content …`、`Host scroll strip …`）。
- 页面容器内不能画自定义图形（矩形、线段、图片）：`ImDrawList_*` 从插件调用会卡死
  渲染线程。
- 游戏外观按钮没有点击音效，悬停是瞬时换色（主菜单自己带过渡动画）。
- 不能热卸载、不能运行时增删页面，只能重启。
- 取光标在窗口内的位置、控件矩形范围：`igGetCursorPos`、`igGetWindowPos`、
  `igGetItemRectMin/Max` 在本构建返回 `0x80000000`，SDK 刻意不暴露。
- 多显示器（把窗口拖到另一台不同缩放的显示器）未测；本机 175% 缩放桌面上的几何与点击
  已经跑通，见上面的显示小节。

## 还没做的

- 键盘按键、字符输入与中文输入法已实测（自动 + 人工各一轮）；窗口尺寸变化后的几何与
  点击已自动断言，175% 缩放的桌面也已覆盖。剩下的只有"换到另一台缩放的显示器"。
- 电路板侧栏插槽目前只有一个位置（右侧）一种形态：没有折叠／隐藏按钮，也没有和游戏
  自身工具栏的位置协商；面板被宿主缩放到窗口内。
- 侧栏插槽自身没有做键盘导航与 IME 测试，也没有做多显示器／DPI 变化回归。
