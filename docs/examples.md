# 随附示例

示例源码在 `examples/`，打包产物在 `dist/`。除 `example.menu-demo` 外都需要 MinGW-w64
构建（`build.ps1` 会一并编译并打包）。

## example.drawing-demo — 自定义绘图

安装 `dist/example.drawing-demo.mod` 并启用，重启后从主菜单 **Custom drawing** 打开页面；
电路板上按 **F8** 可打开浮动面板。拖动画布调整贝塞尔控制点，松手增加点击计数；
可切换网格、调整线宽、重置。示例同时展示圆角矩形、圆、三角形、渐变、凹多边形、
折线和嵌套裁剪。源码 `examples/drawing-demo/plugin.cpp`，接口见 [sdk/ui.md](sdk/ui.md)。

## example.board-panel — 电路板侧栏面板

`registerBoardPanel` 的最小示例：在电路板右上角注册一个宿主拥有的侧栏面板，内容是
Ping 按钮、复选框、24 行需要滚动才能看完的列表、波形画布与一行宿主归属说明。面板随
关卡出现和消失，落在面板上的点击不会落到电路板；内容超出面板时被裁掉，并可用右缘的
滚动条拖动查看（机制见 [sdk/ui.md](sdk/ui.md#电路板侧栏插槽-tcuiregisterboardpanel)）。

- 用途：从"插件自己管理浮动窗口"改成"宿主提供插槽"的最短路径；也展示插槽里能直接用
  `tc_game_ui` 外观按钮和 `tc_ui_draw` 画布。
- 它不需要 Hook：示例包 `hooks=0`，面板完全由加载器调度。
- 真机回归：`tests/ui-board-panel-playtest.ps1`（先 `-Probe` 记录几何，再默认模式发真实
  点击；`-Example` 只验证发行包注册成功）。
- 源码 `examples/board-panel/plugin.cpp`。

## example.waveform-demo — 关卡波形面板

把**当前关卡自己的输入与输出**按周期画成波形，并把同一份数据导出成 VCD 或图片。

- 数据：`tc::trace::Sampler`（[sdk/tc_trace.h](sdk/simulation.md#关卡波形tc_traceh)）。
  只读，不碰仿真状态；槽位在运行期按"哪些字节在动"发现，数量按关卡声明的引脚数取，
  整段没动过的引脚会退回按间距推断并在界面标注 `(slots estimated)`。
- 界面：注册为电路板侧栏面板 `waveform`，每条输入／输出一行方波泳道（输入蓝、输出黄），
  带 8 周期时间网格，只显示最近 N 个采样，随关卡出现与消失。
- 按钮：**Export VCD** 写标准 VCD（GTKWave／Surfer 可打开）；**Export image** 用
  `glReadPixels` 抓当前帧缓冲写成 32 位 BMP；**Clear** 丢弃已有采样重新开始。
- 只跟着仿真走：`sample()` 只在周期变化时追加一行，暂停时波形停住（头部显示
  `paused (waveform held)`），一帧一行变成**一个周期一行**。仿真跑得比渲染快时（如"连续
  运行"）会跨周期，VCD 里能看到时间戳跳跃；要让每个周期都被画出来需要下一步的逐周期采样。
- 指定监测目标：每条输入／输出一个复选框，未勾选的信号不画、也不进 VCD（数据仍保留），
  下方显示 `watching 2/2 in, 2/2 out`。
- **导线探针**：在电路板上点选一条导线，按面板的 **Probe wire** 把它加成一条绿色泳道
  （同时进 VCD，变量名 `p0`…）；**Clear probes** 清空，每条探针可单独勾选显示。
  取值范围是那条网络的仿真状态槽，真机断言它与关卡自身输出逐周期一致。
  （元件的**引脚**探针、字宽网络的位宽核对还没做，见
  [research/waveform-handoff.md](research/waveform-handoff.md) §4。）
- 图片为什么必须由插件自己抓：本构建是独占翻转的置顶 GL 窗口，`PrintWindow` 与桌面抓屏
  都拿不到像素（详见 [verification.md](verification.md#波形面板真机数据回放2026-09-18)）。
- 真机回归：`tests/waveform-playtest.ps1`（默认驱动模式自己进入 `and_gate` 并运行；
  `-Example` 只验证发行包注册）。源码 `examples/waveform-demo/plugin.cpp`；完整现状、
  实测数据与未完成项见 [research/waveform-handoff.md](research/waveform-handoff.md)。

## example.cycle-guard — 周期运行守卫

“Hook 游戏 `sim_do`，限制连续运行长度”的完整示例。

- Hook `sim_do`，在命令到达仿真线程前修改目标周期：把连续/长运行限制为
  “当前周期 + N”，可选 1、10、100、1000。
- 保留暂停、重置以及不超过上限的单步/短运行命令。
- 面板显示实际周期、拦截次数和修改前后目标，提供批量运行按钮；`F6` 显示/隐藏面板。

用法：

1. 进入已有电路后，先用一次游戏原有的运行/单步按钮，让插件捕获该电路的仿真上下文
   （未捕获时面板的运行按钮为灰色）。
2. 选择 100，点击“请求连续运行（验证拦截）”：仿真应在最多再执行 100 个周期后停下
   （关卡本身也可能更早停止）。
3. 取消面板里的“拦截游戏运行命令”，后续命令恢复原行为。

边界：只拦截经过 `sim_do` 的运行命令，不保证覆盖所有编译/加载路径或其他 Mod 直接访问
内核的行为；不修改电路文件。源码 `examples/cycle-guard/plugin.cpp`。

## tcmod.mod-inspector — 只读状态面板

`tc_ui.h` 的最小示例：一个常驻面板，显示战役、存档份数、当前周期、内置／自定义原型数量、
选中的元件与导线数、当前关卡路径，以及视口尺寸与鼠标坐标。

- 用途：确认加载器与 `TCMod` 聚合模型在真实游戏里读到的东西；也是自己写面板的起点。
- 界面：面板固定贴右上角（`tc::ui::topRight`），`F7` 显示／隐藏。
- 编写方式见 [sdk/ui.md](sdk/ui.md)；真机回归 `tests/ui-playtest.ps1` 会断言它的面板确实
  在 ImGui 帧内绘制，并打印一次视口分辨率作为证据。

## example.circuit-and — 电路封装元件

启动时用内存中的 `circuit.data` 注册一个两输入一输出、位宽为 1 的 `AND2 Test`
自定义元件，复用内置 AND 的外观图标。

- 正常启动后，在 Sandbox 或 Foundry 打开元件列表即可放置；部分战役关卡会按游戏规则
  禁止自定义元件。
- 示例同时写入定义头里的设计统计 `(1 门, 1 延迟)`——元件的“延迟”就是设计自身的
  关键路径，放置后整板延迟按声明值进入编译统计。
- 对照实验：把 `tc-modloader-data/plugin-data/example.circuit-and/design-stats.txt`
  写成 `1 5` 再重启，元件信息显示 5，单件板子的总延迟也是 5。

## example.custom-or — 行为由 C++ 决定

导入一个占位电路定义，再用 `register_logic` 注册回调：元件每周期行为由 C++ 决定，
关卡判定、内置元件、暂停与重置仍由游戏执行。

- 真机回归 `tests/native-logic-playtest.ps1` 共 11 个场景，覆盖 1 进 1 出、3 进 1 出、
  3 进 2 出、8 位字宽、2×8 位、3×8 位、混合位宽（8+3、1+8+8→8+1）、多实例与
  内置门混用。
- 机制、形状规则与实现约束见 [sdk/custom-logic.md](sdk/custom-logic.md)；教程见
  [guides/component-with-cpp-logic.md](guides/component-with-cpp-logic.md)。

## example.byte-adder — 单字节加法器（1 门 / 1 延迟）

注册一个 **进位入(1 位) + A(8 位) + B(8 位) → 和(8 位) + 进位出(1 位)** 的元件，
行为由 C++ 回调决定，定义里声明的设计统计固定为 **1 门 / 1 延迟**：

```text
sum       = (carry_in + A + B) & 0xFF
carry_out = (carry_in + A + B) >> 8
```

- 用途：验证“声明 1 门 1 延迟”在元件面板与板级编译统计里的表现，同时验证字宽引脚的
  C++ 回调（8 位输入、8 位 + 1 位输出）。
- 测法：装包 → Sandbox/Foundry 放置 `Byte Adder` → 面板应显示 1 门 / 1 延迟
  （加载器用声明值覆盖内部电路的展开计数，否则该占位电路会显示 51 门）；
  用战役关卡 **Byte Adder**（`campaign/byte_adder`）做端到端判定，该关卡自己比较
  `carry_out << 8 | sum` 与 `carry_in + A + B`。
- 真机统计：`tests/component-cost-playtest.ps1` 的 `observe` 模式（板上元件由本 Mod
  提供）在 `byte_adder` 关卡上读到 `compiled gates=1 delay=1` 与界面
  `build_scores gates=1 delay=1`，报告 `build/byte-adder-realmod-report.txt`。
- 源码 `examples/byte-adder/`；包 `dist/example.byte-adder.mod`。
- 真机自检（开发用）：在 `plugin-data/example.byte-adder/autotest.txt` 写关卡名
  （例 `byte_adder`）后启动，日志会打印前 8 个周期的输入/输出和最终判定状态。
  已验证的日志片段：

  ```text
  Native logic: registered custom 0x414444385f303031 inputs=3 outputs=2 in0=(-2,-1,w1) in1=(-2,0,w8) in2=(-2,1,w8) out0=(2,0,w8) out1=(2,1,w1)
  byte-adder: cycle=3 carry_in=1 a=195 b=134 sum=74 carry_out=1     # 1+195+134 = 330 = 256+74
  byte-adder: autotest finished cycle=39 verdict=0                   # 关卡 40 周期无失配
  ```

## dev.cost-watch — 代价与延迟观察（只读）

挂钩游戏的代价/分数函数，把界面实际显示的 `gates/delay`、各 kind 的返回值与原型字段
周期性写进加载器日志（前缀 `cost-watch:`），不修改任何游戏状态。排查“延迟没被计入”
一类问题时使用。

注意：加载器自己占有 `get_gate_cost` 与 `preorder` 这两个 Hook，同一次启动里
`dev.cost-watch` 会跳过它们（日志 `cost-watch: get_gate_cost hook unavailable ...`
与 `observing 4/6 targets`），其余观察继续工作；门数观察改用
`tests/component-cost-playtest.ps1` 的探针（`observe` 模式下元件由真实 Mod 提供，
探针只读编译统计与界面分数）。

## dev.kind-list — 内置元件表转储（只读）

启动后枚举 `PROTOTYPES`，把 125 个内置元件的 kind、名称、引脚数与引脚偏移写进
`tc-modloader-data/plugin-data/dev.kind-list/kinds.txt`；`tests/kind-list-playtest.ps1`
会把它保存为 `build/kinds.txt`。写夹具、推算引脚偏移时查它。

## dev.menu-demo — 主菜单页面

原生 `format: 2` 示例：注册一个主菜单页面，演示页面容器、同名控件的 Mod ID 隔离和返回主页。
源码在 `examples/menu-demo/`，发布包为 `dist/dev.menu-demo.mod`；同源的 peer/driver 构建只用于
真机测试，不随发布分发。纯资源 `format: 1` 的格式与例子见 [mod-format.md](mod-format.md)。

## 开发期探针（不随发布分发）

| 探针 | 源码 | 用途 |
|---|---|---|
| `dev.sim-state` | `tests/sim-state-probe.cpp` | 仿真状态映射、整板解释器实验（弃用路径） |
| 元件放置/持久化/代价探针 | `tests/component-*-probe.cpp` | 菜单放置、保存重载、代价字段核对 |
| fixture 生成器 | `tests/and-component-fixture.cpp` | 生成元件定义、关卡原理图与对照板 |

这些探针只在隔离沙箱中使用，结论见 [verification.md](verification.md)。
