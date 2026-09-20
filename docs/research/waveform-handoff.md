# 关卡波形面板与仿真数据回放：交接文档（2026-09-18）

本文只讲一件事：**把当前关卡的仿真数据画成波形**这条线，做到哪一步、证据在哪、下一步
该怎么接。面向接手的人，不要求读过之前的对话；所有结论都标了证据来源，未验证的地方
明确写成"未验证"。

## 0. 一句话现状

面板能把**当前关卡声明的输入/输出**按周期画成方波，只在仿真跑动时推进，可导出 VCD 和
图片，能在真机上被断言；**导线探针已经可用**：在板上选中一条导线 → 面板 **Probe wire**
按钮把它加成一条绿色泳道（同时进 VCD），真机断言它与关卡自身输出逐周期一致。
元件引脚的探针、字宽网络的宽度、逐周期采样仍未做（见 §4）。

## 1. 交付物与文件地图

| 文件 | 作用 | 备注 |
|---|---|---|
| `sdk/tc_trace.h` | `tc::trace::Sampler`：只读采样关卡 I/O、槽位发现、VCD 导出、监测目标可见性 | 插件唯一入口，不含任何写操作 |
| `examples/waveform-demo/plugin.cpp` | 发行示例：电路板侧栏面板 + 泳道绘制 + 三个按钮 + 进程内抓帧 | `mod.json` 同目录，`hooks=0` |
| `tests/trace.cpp` | 离线单测：槽位发现、数值、稀疏回退、VCD 文本、**周期门控**、可见性 | `build.ps1` 内编译并执行 |
| `tests/waveform-driver.hpp` | 真机驱动 `TC_WAVE_DRIVER`：自己进关卡、编译、运行、请求导出/抓帧、暂停对照、`TC_WAVE_DUMP` 板表 dump | 只编进测试包 |
| `tests/waveform-playtest.ps1` | 真机用例：断言数据、暂停不增长、逐周期、VCD、画面像素 | 默认驱动模式；`-Example` 只验发行包 |
| `tests/sim-trace-probe.cpp/.ps1` | 上一轮的定位探针：两份 I/O 历史缓冲的槽位与序列 | 槽位布局的原始证据 |
| `build.ps1` | 打包 `dist/example.waveform-demo.mod` 与 `dist/dev.waveform-demo-driver.mod` | 见"波形示例"注释块 |
| `docs/examples.md` / `docs/sdk/simulation.md` / `docs/verification.md` / `docs/changelog.md` | 面向使用者的文档、SDK API、验证记录、版本历史 | 与本文配合阅读 |

面板依赖的宿主能力来自更早的 UI 插槽工作：`tc::ui::registerBoardPanel()`、宿主
`register_ui_slot`、以及"在游戏自己采样鼠标之前绘制面板"的插入点（`build_board_ui` 的
`igIsAnyItemActive` 调用，返回 RVA `0x46b593`）。细节见
[ui-next-steps.md](ui-next-steps.md) 与 [../sdk/ui.md](../sdk/ui.md)。

## 2. 已经实现并验证的行为

### 2.1 数据来源

关卡的 I/O 历史在游戏自己的两份缓冲里：`simulation_input_replay`（输入）与
`simulation_output_history_pins`（输出），每个元素一个 64 位槽。采样器**不写死偏移**：
它扫描缓冲里"动过的字节"得到槽位，数量以 `TCGameStateModel` 声明的引脚数为准；某个引脚
整段没动过时按相邻槽位间距推断，并用 `assumedStride()` 明说（界面显示
`(slots estimated)`）。实测 `and_gate`（2 输入 2 输出）：输入槽 `0/8`、输出槽 `55/64`。

### 2.2 只跟仿真走：一个周期一行

`Sampler::sample()` 只在**周期变化**时追加一行；暂停、关卡还没开始跑时一行都不加，
返回 `false`。真机断言（`tests/waveform-playtest.ps1`）：

```
DRIVER: paused rows=1
DRIVER: paused rows after 2s=1     ← 暂停 2 秒，一行都没加
DRIVER: row 2 cycle=0 in=0,0 out=0,0
DRIVER: row 3 cycle=2 in=2,2 out=0,0
DRIVER: row 4 cycle=3 in=3,3 out=1,1   ← cycle 严格递增
```

**已知分辨率限制**：采样发生在渲染线程、每帧一次，所以仿真跑得比渲染快时（"连续运行"）
两次采样之间会跨过多个周期，VCD/界面表现为时间戳跳跃。要每周期都不漏，见 §4 P0。

### 2.3 指定监测目标（当前范围：关卡引脚）

面板给每条输入/输出一个复选框；未勾选的信号**不画、也不写进 VCD**，但仍继续采样，
勾回来即可。`setVisible(isInput, pin, bool)` 是 SDK 接口，`visibleInputs()/visibleOutputs()`
给出计数。注意面板布局：画布排在控件**之前**——控件排在画布前面会把画布挤出宿主给的
内容高度（踩过：波形整块消失，像素断言直接变 0）。

**引脚数量按电路板数，不按游戏全局量**：`level_used_input` / `level_used_outputs` 实测会
撒谎（一个 1 进 1 出的电路上仍返回 2/2，面板于是多画两条泳道）。插件改为数板上的 IO 元件
（kind `0x3f` 输入、`0x44` 输出，与自定义逻辑网表构建器同一套），用
`Sampler::setDeclaredCounts()` 覆盖；板上没有 IO 元件时才回退到全局量。日志：
`Waveform: the board has N input(s) and M output(s)`。

**重新开始仿真会清空波形**：`sample()` 检测到周期**倒退**即认为玩家重新开始跑（reset /
重新运行），清掉已有行与探针值、从头记录，并通过 `takeRestartFlag()` 让宿主打一行日志
`simulation restarted - waveform cleared`。否则两次运行的波形会接在一起、X 轴来回跳。

### 2.3b 导线探针（2026-09-18 完成）

- 面板按钮 **Probe wire**：把**当前选中的导线**（用游戏自己的选择集，
  `TCBoardModel::selectedWireIdAt`）加成探针目标；**Clear probes** 清空。每条探针一条
  绿色泳道，可单独勾选是否显示，也会写进 VCD（变量名 `p0`、`p1`…，宽度随网络）。
- 数据来源：导线记录 `+0x38` 是它在仿真状态缓冲里的**字节偏移**，`+0x30` 是位宽；
  取值就是游戏自己的 `sim_state_read_bits(offset, width)` = `read_u64(offset) & ((1<<width)-1)`。
- 板模型（要读导线表）由一个**独占但没人用的 Hook** 捕获：`load_level__modelZutilities_u7740`
  的第一个参数就是模型，关卡加载时调用一次。装在别处会抢 `handle_update_wire`
  （wire-palette 用）或 `sim_do`（cycle-guard 用），见 §3.3。
- **采样时刻很关键**：状态是仿真步进结束时才写好的，所以在"周期刚变化"的那一帧读探针会
  读到上一个周期的值（实测：关卡输出已经是 1，探针还是 0）。因此 `sample()` 现在**把行
  延后一次调用落盘**：捕获时读关卡 I/O，落盘时读探针。仍然是一周期一行、暂停不长。
- 真机断言（`tests/waveform-playtest.ps1`）：驱动器挑"接在关卡输出侧的那条导线"加探针，
  逐行比较 `probe value` 与关卡自己的输出历史，必须完全一致（and_gate：`0,0,0,1`）。

### 2.4 导出

- **Export VCD**：标准 VCD，1 cycle 一步，只写被勾选的信号。
- **Export image**：`glReadPixels` 抓当前帧缓冲写 32 位 BMP（见 §2.5）。
- 落盘位置：`<游戏>/tc-modloader-data/plugin-data/<mod-id>/`。

### 2.5 为什么截图必须由插件自己抓

本构建跑的是**独占翻转（independent flip）的置顶 GL 窗口**：`PrintWindow(hwnd, dc, 0)`、
`PW_RENDERFULLCONTENT`、`CopyFromScreen` 在真机上都拿不到游戏画面（前者纯黑，后者抓到的
桌面里没有游戏窗口；窗口 `visible=True, iconic=False, cloaked=0`）。仓库里更早的 UI 用例
写出的 `menu.png` / `driver-page-open.png` 因此也是纯黑。
结论：**要画面就在进程内 `glReadPixels`**。抓帧发生在 ImGui 帧构建期间，读到的是
**两帧前**的画面；面板每帧都在画，所以正是想要的图。驱动因此把抓帧请求安排在最后一行
到达后的第 4 帧（约 60 ms），既包含最后一步，又赶在关卡结束/切场景之前。

## 3. 实测事实（可复用的数字）

### 3.1 关卡 I/O 槽位

| 关卡 | 输入槽 | 输出槽 | 来源 |
|---|---|---|---|
| `and_gate`（2 进 2 出） | `0`、`8` | `55`、`64` | `tests/sim-trace-probe.*` 真机 dump + `tests/trace.cpp` 复现 |

序列与关卡自带测试一致：输入 `0,1,2,3`、输出 `0,0,0,1`（cycle `-1,1,2,3`）。

### 3.2 板内导线表与状态槽（`TC_WAVE_DUMP=1` 实测，已被探针使用）

- 元件序列：`model+0x78`（长度）、`model+0x80`（数据，步长 `0x238`）；导线序列：
  `model+0x98` / `model+0xa0`（步长 `0x68`）。
- 导线记录两端 int16 坐标在 `+0x18` / `+0x1c`；`get_wire__presenterZutilitiesZhelper95functions_u1916`
  的返回 id **就是导线在序列里的下标**（`and_gate` 内置解实测三条：`id(a)=0,1,2`）。
- 每条导线记录 `+0x38` 是**状态字节偏移**（`and_gate` 内置解的三条导线：
  `256` / `258` / `260`），`+0x30` 是位宽（这三条都是 1）。
- `sim_state_read_u64(offset)` 就是 `*(uint64*)(state_base + offset)`（反汇编核对：
  `add simulation_state,%rcx; mov (%rcx),%rax`），`sim_state_read_bits(offset,width)` 是它
  取低 `width` 位（同一段反汇编）。所以**导线的值 = `read_bits(offset, width)`**。
- 一个坑：状态是仿真步进**结束时**才写的。在"周期刚变化"的帧里读，会读到上一周期的值；
  实测把这一帧的读数与"暂停后安定下来"的读数对比才知道（前者 0、后者 1）。采样器因此把
  行延后一帧落盘（见 §2.3b）。
- 复现方式见 §5 的 `TC_WAVE_DUMP=1`（会打印导线表、`+0x38`、`get_wire` 返回、`read_u64`、
  `read_bits`、以及"0..700 里哪些位是 1"）。

### 3.3 Hook 是独占的（踩过两次）

`TCHost::create_hook` 对同一目标只允许一个所有者，第二个请求返回"Hook rejected: phase,
target or conflict"。后果与已做的处理：

- `dev.cost-watch`（开发探针）曾抢占 `handle_update_wire`，导致玩家 Mod
  `local.wire-palette` 加载失败（日志里 9/17 起成对出现，顺序一变就成败不定）。现在探针
  不再 Hook 该目标；报告里的 board 行固定为 `no context yet`。
- 因此**发行版波形示例一个 Hook 都不装**（`hooks=0`）：它只用加载器给的板侧栏插槽。
  后续做节点探针时必须避开 `handle_update_wire`、`sim_do`、`handle_place_wire`、
  `add_wire_from_pos` 这些已被别的 Mod 使用的目标，或者改由加载器统一提供模型指针。

## 4. 未完成项与建议做法

### P0-a 逐周期采样（让波形不漏周期）

现状：一帧一次，快速运行时跨周期。建议：

1. Hook 每周期步进（`jit_function__modelZsimulationZsimulator95functions_u84`，
   VA `0x14021cd40`，见 [component-pipeline.md](component-pipeline.md) §12.1 的符号表）；
2. 在**仿真线程**把 `simulation_input_replay` / `output_history_pins`（以及将来的探针槽）
   压进环形缓冲；
3. 渲染线程只做"取走新行"，用无锁队列或临界区（周期频率可能上千 Hz，别做分配）；
4. 该 Hook 要装在加载器里（或至少由加载器持有），避免再出现 §3.3 的所有权冲突。

验收建议：用 `example.cycle-guard` 把运行限制到 N 周期，断言波形行数 == 该段周期数，
且 `cycle` 连续无跳号。

### P0-b 内部节点（导线）探针

已完成（见 §2.3b）。剩下的细化：

1. **位宽**：目前宽度取自导线记录 `+0x30`，只在 1 位网络上验证过（`and_gate` 内置解）。
   需要用一个字宽关卡（例如 `double_number` + `nl_def_double8.data`）核对：记录里哪个字段
   是位宽、`read_bits(offset, 8)` 是否给出 0..255 的字值；
2. **失效处理**：电路被编辑/重编译后，旧探针的偏移可能失效。目前只是继续读（值可能不再
   有意义）。应当记录探针的导线坐标，重编译后按坐标重新解析，解析不到就把该探针标灰；
3. **元件引脚探针**：游戏自己的 watcher 是"元件 watchee"路径（`get_sim_state` →
   `get_state_index`），把元件引脚的偏移也读出来，就能监视任意引脚而不只是导线；
4. **拾取体验**：现在要点"Probe wire"按钮；可以改成边沿检测选中集变化自动添加，或在板上
   点一根线就加（避免按钮）。

### P1 其它

- 元件引脚探针（需要引脚布局：`native_logic.hpp` 的 `readPinGeometry` 与
  `component-pipeline.md` §12.2 的实测槽位表可作起点）。
- 字宽关卡（`symphony_*` 32 位 IO）与更多关卡的验收；目前只验过 `and_gate`。
- 波形交互：缩放/平移/游标、导出 CSV、双游标测宽。

## 5. 复现与调试命令

```powershell
# 全量构建（含 tests/trace.cpp 单测）
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 真机用例：默认驱动模式自己进入 and_gate 并运行
powershell -NoProfile -ExecutionPolicy Bypass -File tests/waveform-playtest.ps1
# 只验证发行包注册与加载
powershell -NoProfile -ExecutionPolicy Bypass -File tests/waveform-playtest.ps1 -Example

# 上一轮的槽位定位探针（需要 build/and2_solution_builtin.data）
powershell -NoProfile -ExecutionPolicy Bypass -File tests/sim-trace-probe.ps1
```

想看 §3.2 的板表 dump：在 `build/waveform-sandbox` 里自己起一次驱动包并带上环境变量
`TC_WAVE_DUMP=1`：驱动器在开始运行前与最后一行到达后各 dump 一次整张导线表（坐标、
`+0x38` 字段、`get_wire` 返回值、`read_u64`、`read_bits(1/2/4/8)`、以及"0..700 里哪些位
是 1"），并且面板每出现一行新波形也会 dump 一次（最多 8 行）——跨周期对比就是靠这个
看出"哪些位跟着哪条线动"的。

失败时先看：

| 症状 | 先查 |
|---|---|
| 面板不出现 | 加载器是否为含 `registerBoardPanel` 的版本（日志有 `UI slot registered: waveform (board side panel)`）；`mod.json` 是否启用 |
| 波形一直空/不动 | 是否在关卡里；`Waveform: resolved slots …` 是否出现；仿真是否在跑（头部 `simulating`） |
| 波形整块不见 | 面板内容是否被控件挤出可视区（§2.3 的布局坑） |
| 截图全黑 | 是否走了进程内 `glReadPixels`（§2.5） |
| 某个 Mod 加载失败 | 是否与其它 Mod 抢同一个 Hook（§3.3） |

## 6. 现用安装状态与回滚（D:\p）

| 项 | 值 |
|---|---|
| 加载器 | `D:\p\game_engine.dll` = `dist/tc-loader.dll`，SHA-256 前缀 `9C7167BD95AF6184`（4337736 字节，2026-09-18 18:12） |
| 旧加载器备份 | `D:\p\game_engine.dll.bak-20260918-before-waveform`（`35BAAA450DBFC0E0`） |
| 新增 Mod | `D:\p\mods\example.waveform-demo.mod`（已 `apply`，enabled 列表含 `example.waveform-demo`） |
| 同时刷新的 Mod | `dev.cost-watch.mod`（去掉 `handle_update_wire` 的 Hook）、`local.wire-palette.mod`（换成仓库当前构建） |
| 旧包备份 | `D:\p\tc-modloader\build\live-backup\dev.cost-watch.mod.20260917`、`local.wire-palette.mod.20260916` |

回滚：把备份改名回 `game_engine.dll`，再把 `build/live-backup` 里的两个 `.mod` 放回
`D:\p\mods\` 并 `dist\tcmod-cli.exe D:\p apply <当前 enabled 列表>`。玩家的存档没有被改动；
Mod 的存档在 `%USERPROFILE%\AppData\Roaming\Turing Complete Mods\profiles\…`。

## 7. 一趟"最小改动"的下一步（建议直接照做）

1. `docs/research/waveform-handoff.md`（本文）读 §3.2 与 §4 P0-b；
2. 写一个一次性探针（可以照 `tests/sim-trace-probe.cpp` 的骨架），加载 `not_gate` 的内置解，
   每周期 dump `read_bits(k,1)`（k 扫 0..700）与关卡自身的输入/输出，找相位差；
3. 用找出的换算更新 `sdk/tc_trace.h`（新增 `addBitProbe(label, bitIndex, width)` 之类），
   在 `tests/trace.cpp` 里补离线断言，在 `tests/waveform-playtest.ps1` 里补一条"探针随信号
   变化"的真机断言；
4. 面板加"拾取导线"按钮 + 目标列表（含删除），导线标签用坐标 `(x,y)`；
5. 最后再考虑 §4 P0-a 的逐周期采样——它会把"跨周期"从已知限制里去掉。
