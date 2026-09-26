# 游戏服务与编辑地基交接

更新时间：2026-09-20

> **接手结论**：六项上层地基（版本化服务表、Board 读快照、Component/Wire 句柄、命令总线、
> 生命周期事件、事务/撤销/保存）已全部实现，并各自有真机证据；此后又追加了 Board V5 引脚、
> V6 连接判定、`tc.simulation` 读数服务与编辑族的第一个命令（元件放置）。
>
> **真机探针一共纠正了两处契约**，都记在正文「本轮修正」一节：导线记录是两个端点（不是"一个
> 端点的 x/y"），以及游戏撤销会留下 kind 0 的墓碑（所以"元件数"必须跳过 kind 0）。
>
> 下面「尚未验收与风险」共 10 条，其中第 1、2、3 条是接手后最先要处理的。

## 本轮目标

按顺序完成六项地基，并让它们**各自独立演进**、互不破坏：

1. `query_service` 与版本化服务表；
2. `TCBoardApiV1` 与只读 Board 快照；
3. 从快照签发 Component/Wire 句柄；
4. 命令总线及完成事件；
5. Board 生命周期与变更事件补全；
6. 事务、撤销与保存集成。

六项跑完后追加的方向（同样是"有证据才进契约"）：

- Board V5：元件引脚（相对坐标／位宽／`AUTO_SIZE`）；
- `tc.simulation`：周期／帧／状态缓冲 + 按偏移读值；
- Board V6：导线两端连到哪个元件的哪个引脚；
- 编辑族第一个命令：`TC_COMMAND_BOARD_PLACE_COMPONENT`（命令 V2 + 事务 V2）；
- 撤销/重做语义实测、`OBJECTS_CHANGED` 真机化。

## 已实现

### 公共 ABI

| 接口 | 版本 | 内容 |
|---|---|---|
| `tc.board` | V1–V6 | V1 身份/验证/解析；V2 值快照；V3 对象枚举（调用方缓冲）；V4 对象读取（元件/导线）；V5 元件引脚；V6 导线端点连接判定 |
| `tc.commands` | V1/V2 | V1 六个基础命令；V2 追加载荷（`custom_prototype_id`/`kind`/`rotation`/`x`/`y`）与 `PLACE_COMPONENT` |
| `tc.transactions` | V1/V2 | 乐观冲突预检、批处理、`SAVE_ON_COMMIT`；V2 的 `stage` 接受 `TCCommandV2` |
| `tc.lifecycle` | V1 | Board 进入/离开、对象结构变化、选择变化（值类型事件） |
| `tc.simulation` | V1 | `get_state`（周期/帧/状态缓冲大小）+ `read_value`（低位掩码读值） |

- `TCHost` 只尾追加过一个字段：`query_service`（V1–V6 全部经由它分发）。旧二进制 Mod 的兼容在
  本分支收尾时**手动**验证过一次（用改动前的 SDK 头编译插件、按旧能力名打包、让新加载器加载，
  同进程的新旧 Mod 都正常），但没进测试套件（见风险 9）。
- 句柄 kind：`BOARD`／`COMPONENT`／`WIRE` 已签发；**`LEVEL` 仍预留**，且按下面「下一步」的
  结论，它更适合做值快照而不是句柄。
- 能力位新增 `TC_CAP_SERVICES`（`1<<13`），包清单名字 `services`。

### Loader 实现

| 文件 | 职责 |
|---|---|
| `src/services.hpp` | 服务分发：`tc.board` 六版、`tc.simulation`，以及各读取器的绑定 |
| `src/board_objects.hpp` | Board 对象表读取：序列长度/数据指针、元素成员判定、元件与导线字段解码 |
| `src/board_pins.hpp` | 内置原型表的**安全枚举**（未知 kind 绝不交给游戏）与引脚条目解码 |
| `src/board_edits.hpp` | 0x238 放置记录模板与命令载荷映射 |
| `src/board_connect.hpp` | 引脚几何匹配与邻近窗口 |
| `src/simulation_read.hpp` | 状态缓冲界内检查与掩码算术 |
| `src/native.hpp` | 读入口实现、命令/事务执行器、生命周期派发、启动期符号绑定 |

启动时加载器会打印这几行，接手第一件事就是看它们是否都为 `armed`：

```text
Symbol profile: 23/23 aliases resolved
Board snapshots: selection counters armed
Board pins: prototype reads armed, 125 built-in kinds
Simulation reads: armed, state buffer 10240000 bytes
Board edits: component placement armed; wire placement stays out of the command bus (...)
```

### 测试与文档

- 离线单测（`build.ps1` 内执行，7 个）：`game-model`、`game-handles`、`services`、`board-objects`、
  `board-pins`、`simulation`、`board-edits`。
- 真机用例（游戏层）：`game-handle-probe`（Board V1–V6、命令、生命周期、事务、仿真、连接、编辑）、
  `component-placement`（菜单放置 + V5 引脚 + 命令放置 + 两步事务 + undo/redo）；
  `kind-list` 提供 `build/kinds.txt`，是引脚偏移的独立基准。
- SDK 文档：`docs/sdk/services.md`（V1–V6 + simulation）、`commands.md`、`lifecycle.md`、
  `transactions.md`。研究日志：`docs/research/board-object-fields.md`（含尚未核实的线索）。
- ABI 基线 652 条记录，相对 HEAD 只有新增。

## 已验证（真机证据行）

```text
PROBE: board v4=4 prefix=1 / v5=5 prefix=1 / v6=6 prefix=1
PROBE: objects status=0 components=3 wires=1 written=4 resolved=4 generation=...
PROBE: component kinds 0:1 60:1 68:1
PROBE: wire info status=0 read=1 of=1 ... xy0=9,0->-9,0 width0=1 slot0=256
PROBE: wire ends status=0 end0=(9,0,dir0,pin0,kind=0x44) end1=(-9,0,dir1,pin0,kind=0x3c)
PROBE: sim state status=0 cycle=-1 frame=134 state-size=10240000 flags=7
PROBE: sim value status=0 raw-status=0 slot=256 width=1 value=0 raw=0 masked=0 agree=1
PROBE: child read next-frame component=-3 wire=-2
component placement custom pins status=-7 inputs=2 outputs=1 kind=0x4e
component placement pins p0=i(-1,-1,w1) p1=i(-1,0,w1) p2=o(2,-1,w1) written=3
component placement command board before=2 after=3 found=1
component placement transaction board before=3 after=5 found=2
component placement undo points=30,0:1 20,6:1 24,10:1 28,10:0
component placement undo components [0x00(0,0)] [0x4e(30,0)c] [0x4e(20,6)c] [0x4e(24,10)c] [0x00(0,0)]
component placement redo state=3 result=0 components=5 live=4 restored=1
PROBE: edit state=3 result=0 live-before=0 live-after=1
PROBE: lifecycle objects-changed=1 selection-changed=0 entered=1 left=0
```

最近一次分层结果：fast 6/6、host 4/4、`game-handle-probe` 与 `component-placement` 全绿。

## 本轮修正的两处契约（真机推翻了原来的读法）

1. **导线记录是两个端点。** `+0x18` 起是 x1/y1/x2/y2 四个 int16，而不是"一个端点的 x/y"。
   三条独立证据：and_gate 内置解的字节是 `(-6,-1)->(-13,-1)`；战役关卡是 `(9,0)->(-9,0)`，
   正好横在输入/输出引脚之间；放置实验里新线的两个端点都等于请求点。`TCWireInfoV1` 已改为
   `x1/y1/x2/y2`（88 字节）。
2. **撤销留下 kind 0 墓碑。** 一次 undo 只回退**一步**（两步事务要两次），被撤掉的槽位变成一条
   kind 0 全零记录、序列长度不变。所以"数元件"必须跳过 kind 0，`component_count` 只是序列长度。

## 尚未验收与风险

1. **选择集写入没有入口，`SELECTION_CHANGED` 也就没有真机覆盖。** 候选符号已经用 `nm` 找出来了
   （`select_component`、`incl`×2、`excl`、`clear_selections`、`get_selection`、
   `add_undo_changes`），但**签名未核实**，见 `research/board-object-fields.md` §5。
2. **命令类型覆盖不均。** 真机断言过的只有 `SIM_STOP`、`SAVE`（事务保存屏障）、`BOARD_UNDO`、
   `BOARD_REDO`、`PLACE_COMPONENT`；`SIM_RUN` 与 `SIM_RESET` 有实现、无真机断言。
3. **LEVEL 与关卡语义未做。** 结论是先做值快照（名字/路径/campaign/IO 数/完成状态）而不是句柄——
   关卡名是关卡加载事件里的借用字符串，句柄语义撑不住。导航命令也还没有。
4. **连接判定的两个假设未在大板上验证**：±32 格窗口够用（实测引脚偏移 ≤±20）、同一点上不会
   有两个引脚。窗口内"先命中即结果"目前只是 fixture 规模的经验。
5. **导线 id 是序列下标**，编辑后会变；不能当长期身份。跨帧身份目前只能靠端点坐标 + 元件/引脚。
6. **kind 0 墓碑的连锁影响**：任何"数元件""按数量做断言"的代码都要跳过它；我们已经在本仓库的
   探针里踩过一次（undo 后 `components=5` 但 `live=3`）。
7. **逐周期采样/回调仍做不到**：EXE 里没有可钩的步进函数（循环在 `compile_asm` 生成的机器码里），
   只能走生成源码注入，已与 `verification.md` 的「仍未做到：逐周期回调」合并为一项独立地基。
8. **编译图层（实例父链、扁平序列、代价、时序）未服务化**：父链已确认属于这一层而不是板块，
   `preorder`／`get_cost`／`add_circuit_code` 都在加载器手里。
9. **旧二进制 Mod 的兼容只在本分支收尾时手动验过一次**（用改动前的 SDK 头编译插件、打包、让新加载器加载），
   没有进测试套件。`TCHost` 只尾追加过 `query_service`，理论上安全，但没有回归护栏。
10. **导线编辑没有进契约**：`add_wire_from_pos` 只"起一条线"（零长度、奇数状态槽、游戏自己的
    `get_wire` 拒绝寻址）。要做真正的程序化连线，需要新的反推或一次钩子归属决策。

## 接手后的第一组命令

```powershell
cd D:\p\tc-modloader

# 1. 完整快速层（含 7 个离线单测、ABI、发布契约）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast

# 2. 宿主层
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier host -NoBuild -KeepGoing

# 3. 两个真机用例（刚构建过时可加 -NoBuild）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier game -Name game-handle-probe -NoBuild -KeepGoing
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Name component-placement -NoBuild -KeepGoing

# 4. ABI 与画像
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compat.ps1 -GameDirectory D:\p
```

## 下一步的线索与建议顺序

> **已确认想要但暂缓的项目单独记在 [PLAN-backlog.md](PLAN-backlog.md)**（撤销分组、让游戏选中、
> 棋盘高亮/标注/镜头），那些是"等你下令"的，不在本节的建议顺序里。
> 按优先级排好的动手清单在 [HANDOFF-next-steps.md](HANDOFF-next-steps.md)。

1. **选择集写入的签名核实**（反汇编 + 真机探针）。若 `select_component` 是 `(board, id)` 这类简单
   签名，写入口与 `SELECTION_CHANGED` 真机化可以同一轮完成。顺带核实 `add_undo_changes`——它如果
   能用，Mod 就能让一批编辑成为**一步**撤销，正好补上"事务 ≠ 一步撤销"的缺口。
2. **#2 的便宜那一半**：IO 引脚清单 + 每根网络的值槽映射（V5 引脚形状 + V6 连接 + V4 槽位），
   不需要采样；关卡自带测试的输入/输出槽（0/8、55/64 那种）属于逐周期那一项。
3. **逐周期采样/回调**（独立地基）：与 codegen 注入同项目，做之前应先出一份研究交接。
4. **编译图观测服务**。
5. `SIM_RUN`／`SIM_RESET` 的真机断言（成本很低，顺手就能补）。

## 踩坑清单（给接手人）

- **`Set-Content` 会把文件写成 CRLF**，本仓库要求 LF（`.gitattributes`）。改完文件务必扫一遍
  行尾；写文件请用 `[IO.File]::WriteAllText(..., UTF8Encoding($false))` 或 `apply_patch`。
  本轮因此修过两次，其中一次还把探针文件写重了 500 行。
- **`tests/*-playtest.ps1` 的 `-Sandbox` 必须给绝对路径**，相对路径会让 `Start-Process` 起不来
  （进程码 `0xC0000142`），表现为"加载器日志都没有"。
- **沙箱每次都要刷新**（`make-ui-sandbox.ps1` 会复制 `dist\tc-loader.dll`），否则会静默测上一版加载器。
- **同帧提交两个编辑会互相打架**：命令在插件回调之后、事务之前执行，所以同帧的放置会让事务的
  基线指纹失效（`CONFLICT`）。探针里要把"改棋盘"排在事务完成之后。
- **`level.load` 帧内的棋盘还没稳定**（实测 3 元件/1 导线 vs 相邻帧 1/0），读数与断言都要等到
  稳定帧。
- **探针的 `result.txt` 必须以 `PASS ` 开头**，playtest 会检查；新增断言优先加在 playtest 里对接
  日志行，而不是只写在探针里。
- **新增离线单测要同时改 `build.ps1`**（编译 + 执行那一对），否则它不会被任何一层跑到。
- 反汇编/符号工作：`nm "Turing Complete.exe" | Select-String ...` 可用；文字级证据优先，
  签名没核实前不要进契约。

## 工作树说明

- 仓库是**大量未提交/未跟踪**的脏工作树（49 项）：六项地基与后续追加都在同一个未发布分支上，
  `VERSION` 仍是 0.6.0。
- 本轮**没有**创建 Git commit，没有覆盖 `dist/releases/0.6.0`。正式发布仍要求干净工作树。
- 不要用 `git reset --hard` 或整树 checkout；需要按文件审阅并保留用户原有改动。
- 如果某个文件被改坏，`git show HEAD:<path>` 取回干净版本再重新施加增量，比在坏文件上继续打补丁更安全
  （本轮对 `tests/component-placement-probe.cpp` 就是这么救回来的）。
