# M4 交接：组合电路与原版复用（四刀已交付，别名判定为不可用）

计划 §16 阶段 4（`PLAN-custom-components.md`）的目标是"让 Mod 明确选择复制元数据、内部复用或完整
别名"，交付四件：**稳定原版类型目录**、**内部网表 builder**、**嵌套依赖**、
**`register_builtin_alias` 可行路径**；验收是"原版 AND 内部节点、嵌套 Mod、状态元件组合以及至少
一个普通原版别名真机通过"。

2026-09-29 一天之内把四件都做完了，下面按"已交付 / 明确不可用 / 仍未做"分开写。

## 0. 现在的状态

| 计划 §16 阶段 4 的交付 | 状态 | 在哪 |
|---|---|---|
| 稳定原版类型目录 | **完成**（第一刀） | `tc.component.compose` V1，见 `docs/sdk/services.md` |
| 内部网表 builder | **完成**（第二刀） | V2 的 `begin/node/connect/commit/abort/status`，编码器 `src/component_netlist.hpp` |
| 嵌套依赖 | **完成**（第三刀） | `node()` 接受另一个已注册类型的 `type_id`，真机导入成功 |
| `create_builtin_node` | **完成** | 就是 V2 的 `node("builtin/…")` |
| `clone_builtin_metadata` | **完成** | V2 的 `clone_metadata`（NAME/DESCRIPTION/COST） |
| `register_builtin_alias` 可行路径 | **判定为不可用**（这正是计划要的"明确返回不可用"） | `alias()` 恒返回 `UNSUPPORTED`；`TC_BUILTIN_FLAG_ALIASABLE` 从不置位 |

真机门禁：`component-compose`（目录）与 `component-composite`（builder/嵌套/复用/拒绝/关卡判分）；
离线：`tests/component-compose.cpp`（拼写与目录 ABI）、`tests/component-netlist.cpp`（编码器与校验，
逐字段对照游戏已接受的夹具字节）。ABI 基线 1673 → 1763。

## 1. 本轮交付：`tc.component.compose` V1（原版类型目录）

| 项 | 位置 |
|---|---|
| 契约（服务 id、结构、标志） | `sdk/tc_service_api.h` 的 `TC_SERVICE_COMPONENT_COMPOSE` 一节 |
| C++ 封装 | `sdk/tc_component_compose.h` |
| 拼写规则（可离线测） | `src/builtin_catalogue.hpp` |
| 读表与查询实现 | `src/native.hpp`：`collectBuiltinCatalogue()`、`compose_*_api`、`queryComponentComposeService` |
| 离线用例 | `tests/component-compose.cpp`（`build.ps1` 内执行） |
| 真机用例 | `tests/component-compose-playtest.ps1` + `tests/compose-probe.cpp`（`dist\dev.compose-probe.mod`） |
| ABI | `tools/abi-snapshot.cpp` + `abi/windows-x64.json`（1673 → 1718 条） |
| 文档 | `docs/sdk/services.md`「tc.component.compose」、`docs/reference/limits.md`「原版类型目录」 |

契约要点：每个 kind 一条记录（名字、说明、声明引脚、两个标志），**稳定拼写** `builtin/<名字>`；
同一构建重名按 **kind 升序**编号 `#2`、`#3`…；没名字的槽位用 `builtin/0x<kind>`；
`find_by_type_id` 先精确、再对**无编号**拼写做 ASCII 大小写折叠。代价与能力位故意不报（理由见
契约注释与 services.md）。

### 本轮实测到的表结构（下一刀会直接用到）

真机一次运行 + `build/kinds.txt` 对照（`tests/component-compose-playtest.ps1` 断言两个独立读表者
125 条全一致）：

- **125 个 kind**：**17 个名字重复**（`Output` ×7、`Input` ×6、`Splitter` ×6、
  `Word Maker`／`Concatenator`／`Maker` ×3，其余 ×2：`AND`、`NAND`、`NOR`、`NOT`、`OR`、`XNOR`、
  `XOR`、`Switch`、`Register`、`Counter`、`Delay Line`）；**21 个槽位无名**（`0x7b` 全零、
  `0x4e` 是自定义实例占的槽）。
- **41 个 kind 声明 0 个输入脚**：有状态族与纯源都在里面（`0x0d`/`0x37` Delay Line、`0x0e`/`0x27`
  Register、`0x26`/`0x4d` Counter、`0x3c`-`0x41` Input…）。它们的输入位置不在原型里。
- **104 个 kind 带 shape**（`TC_BUILTIN_FLAG_HAS_SHAPE`），这是"复制原版元数据"要读的东西。
- **引脚字宽三态**：数字 1–64；`0x7fff…ff` 自动标记；**两个 Splitter 用 `0x7fff…fe`／`…fd`**
  （比自动标记小 1／2，语义未核实，已按 raw 报出）。
- **原型的门数/延迟字段在本构建所有 kind 上都是 0**；真正的代价来自游戏打分器对内部电路的分析
  （实测 Delay Line 5 门 4 延迟，`tests/component-cost-playtest.ps1`）。

### 踩过的坑（写给下一个读 kinds.txt 的人）

- `kinds.txt` 的 kind 是 Nim 的十六进制拼写，**不补零**（`0x4` 就是 `0x04`）；按字符串当键会漏。
- 名字**不是**身份：同一名字可能有 2–7 个 kind，按名字找零件必须带编号。
- 无名槽位也是**合法** kind（`get_prototype` 收）；未知 kind 才会抛 Nim 错误。

## 2. 内部网表 builder：已交付（保留下面的编码事实与设计笔记）

目标：一个 Mod 类型可以**不写回调**，改用游戏自己的零件描述内部电路。计划 §8.2 的服务职责
（建节点、连引脚、设参数、导入 `circuit.data`、校验悬空/位宽/多驱动/组合环、嵌套、返回依赖与诊断）
落在 `tc.component.compose` 的后续版本上。

### 已经量清的编码事实（可以直接照抄）

加载器生成的定义是 v14 的 `circuit.data`（Snappy 只有字面块），`src/component_definition.hpp` 的
`encode()` 已经会写"固定脚手架"：`n` 个输入脚（kind `0x4f`）→ `n` 个收集器（`0x12`）→ `n-1` 个依赖门
（`0x17`）→ `m` 个驱动（`0x12`）→ `m` 个输出脚（`0x51`），再加导线；桥接把**第一个驱动的输入**
换成回调。**测试夹具里已经有一个通用的"任意内置节点 + 任意导线"写手**：
`tests/and-component-fixture.cpp` 的 `addGateAt(writer, kind, x, y)`／`addWire(...)`／`addPin(...)`
——v14 记录字段顺序、导线步进的方向位（`0x8000` 左、`0x4000` 下、`0xc000` 上）都在那里，
而它生成的电路在真机上编译、仿真、计分都通过（`nl-*` 系列夹具）。

### 交付形状（与计划 §8.2 的对应）

- 无回调的纯组合类型：外层输入脚节点 → 内部网表 → 外层输出脚节点，中间没有 collector/driver
  脚手架（那是回调类型才需要的）。`gate_cost` 由游戏按内部电路重算；`delay` 由调用者声明并被
  原样信任（实测）。
- 有回调 + 内部网表混合：**没有做**，仍然不建议：两条路各自的语义（谁先跑、回调看到什么）
  需要单独设计。
- 校验清单：悬空端点、引脚越界、多驱动、组合环（经有状态零件合法）、布线冲突、引用不存在的
  稳定拼写——全部实现并有离线用例；真机用例覆盖"悬空输入被拒"这一条。
- 稳定拼写 → kind 的映射走 V1 目录；Mod 永远不写 kind 字节。
- 节点位置由调用者给，builder 只负责选一条不冲突的路径并拒绝做不到的布局（拒绝信息里带冲突点）。

### 嵌套与别名的结论

- **嵌套 Mod 元件**：已交付。`node()` 接受 `<owner>/0x<id>` 或 `0x<id>`，内层引脚从它自己的原型
  读；内层必须先注册成功（这正是"注册顺序依赖"）。真机：一个组合类型以另一个组合类型为唯一内部
  节点，导入成功、代价 2。**仍未测**：内层属于**别人的 Mod**、或那个 Mod 后被卸载时的语义
  （外层定义已经导入进游戏，内层原型消失后游戏怎么处理）——需要一次专门的测量。
- **`clone_builtin_metadata`**：已交付，但只承诺名字、说明与声明代价。**图标不能复制**：游戏对
  自定义原型忽略 `shape_svg`（`research/component-icons.md` §3 实测），所以契约直接指向 V5
  `set_picture`。真机证据是游戏侧的原型读回（`name="AND" description="[T] when both inputs are [T]"`），
  不是像素对照——"元件栏里长什么样"由 V5 的图片负责，那条路早有像素门禁。
- **`register_builtin_alias`**：**本构建不可用**，`alias()` 恒返回 `UNSUPPORTED`，目录的
  `TC_BUILTIN_FLAG_ALIASABLE` 从不置位。依据：游戏按元件记录的 kind 字节分发，Mod 类型的实例恒为
  `0x4e`；另有两条既有实测同向（自定义原型忽略 shape SVG；默认绘制只能按类型关掉而不能换成原版
  交互）。**没有**逐 kind 的能力位矩阵，因为那需要为每个 kind 找一条"新 id 走原版分发"的入口——
  不存在这条入口时矩阵没有意义。可行的替代是三件套：`clone_metadata` + 同一个原版节点拼的组合 +
  V5 图片。

## 3. 尚未进契约的实测边界（沿用）

- 每脚 1–64 位、总输入 ≤128 位、不做多字信号；内部电路目前只支持**一层占位门**
  （`docs/reference/limits.md`「元件定义」）。
- 有状态元件的输入位置只有 §11.2 的测量（Delay Line `(-3,0)`→`(3,0)`），未进服务。
- 移动/旋转命令、导线编辑仍缺（见 `docs/sdk/commands.md`）；合成鼠标拖动在本构建上不被棋盘接受
  （`docs/research/component-hitbox-path.md`）。

## 4. 验收结果（当时写的清单，逐条对上）

1. **离线**：`tests/component-netlist.cpp` —— 编码器与 `build/nl_def_and3.data`（游戏已接受的夹具
   定义）逐字段对照，加全套拒绝项与"有状态环合法"。（拼写与目录 ABI 在
   `tests/component-compose.cpp`。）
2. **真机**：`component-composite` —— 两个 `builtin/AND` 拼的三输入 AND，放进 `and_gate_3` 关卡，
   游戏自己的测试判 `verdict=1`；`gates=2` 证明游戏按内部电路重算了门数。有状态组合还没做
   （见下面的"仍未做"）。
3. **契约成套**：SDK 头 + `tools/abi-snapshot.cpp` + `abi/windows-x64.json`（1763 条）+
   `docs/sdk/services.md` + `docs/reference/limits.md` + `changelog.md` + `verification.md`，全部
   带真机日志行或离线对照。
4. **版本纪律**：V1 目录表没动，builder 开在 V2（保留 V1 前缀）；`TCComponentComposeApiV2` 里
   clone/alias 是在 V2 首次发布前追加的。

## 5. 仍未做（下一批的起点）

- **5→32 译码器的"布局"（进行中：通道参数已落地，布局待写）**：
  **已交付**：`tc.component.compose` V2 的 `pin_lane(session, cells)`（1–16 格，默认 2 = 历史形状）——
  真机验证 2 入/32 出 + 通道 16 的类型把两列引脚拉到相距 31 格（`in0=(-15,0)`、`out0=(16,-15)`），
  中间 250 单位就是这个译码器要用的画布；连同"同网络总线"（一个源脚扇出可画成干线加分支）。
  **下一步**：给 `Connection` 加可选**路径点**（生成器自己画走廊，builder 只校验+编码），然后把
  `examples/composite-decoder/decoder.hpp` 的布局改成宽通道下的从左到右树形（层级各占一列，输入位
  沿上方走廊分配到各层，输出行按 bit-reversed 顺序对齐右端），离线 `tests/component-decoder.cpp`
  先跑绿，再接 `component-composite` 的真机门禁（导入 + 游戏重算门数 + 32 组合真值表）。
  **现状**：网表生成器与离线真值表模型已写好并通过（65 个零件 / 75 条线，5 个 1 位译码器 + 60 个
  二输入 AND），只是布局还画不出来。`examples/composite-decoder/decoder.hpp`
  已经会生成网表（5 个 1 位译码器 + 60 个二输入 AND，共 65 个零件、75 条线），离线模型对全部 32 个
  输入组合给出正确的 one-hot 真值表；**卡在布线**：这个类型的 5 个输入脚正好落在 32 个输出脚的行
  范围中间（`laneY` 对 5 和 32 都居中），于是任何"从左到右分层"的布局都要横穿输出侧的布线，builder
  按规矩拒绝（拒绝信息给出冲突点）。两条出路，选一条：
  1. **给宽类型改引脚几何**：当输出方向很多时，把输入脚放到输出行带之外（例如上方），输入走廊与
     输出行带分离，分层布局立刻可行——代价是这类类型的引脚位置规则多一条（现有 3 入/1 出等形状保持
     不变，所以既有真机门禁不受影响）；
  2. **做重复树布局**：把每个宽扇出拆成扇出 ≤2 的复制树（32 路复制约 31 个门），整张图变成平面图，
     再按"叶子行 + 父节点居中"的经典树布局摆放；零件数约 119，布线器现有的候选集应该够用。
  离线 `tests/component-decoder.cpp` 已经把真值表钉住（**没有**进 `test-catalog.json`，因为它现在
  在路线检查处失败）。32 输出这一条已经单独量过：`component-composite` 里注册的 2 入/32 出类型
  被游戏读回 `in=2 out=32`（`gates=0`，因为内部只有 ON 源）。

#### 5.1 2026-09-29 订正：卡住布局的是连接规则，不是几何；而且真值表也没过

本节最早写的是"这套网表没有平面画法：父线的两个 AND 中间夹着必须由译码器喂的 AND，摆放怎么换都
消不掉"。**那个结论是错的**，错在对游戏导线的理解上——错把"相触"当成了"同一个网络"。

玩家给的规则（2026-09-29）：**一根导线只有两个端点**；玩家可以在导线中间放一个端点，那根线就
变成两根，而中间那个端点同时是两根线的端点，所以它们的网络相同；**两根导线交叉、交叉处没有端点
时，它们不是同一个网络**。仓库既有的读数支持"导线记录是两个端点"这一半（
[HANDOFF-game-services.md](HANDOFF-game-services.md) 的「本轮修正」第 1 条、
[sdk/services.md](sdk/services.md) 的导线记录表：`+0x18` 起是两对 int16）。

于是 `src/component_netlist.hpp` 里 `routesClash` 现在这条"两根线只要相触就算同一个网络"的规则
**过严**：它把"交叉"与"端点落在别人线上"当成了同一件事，从而拒绝了一批游戏本来就接受的布局。
现行 65 零件网表按这条规则确实画不出来（那条拒绝信息是真的），但它是规则的产物，不是几何的产物。

量了一次：把 `routesClash` 临时改成"永不冲突"，再跑 `tests/component-decoder.cpp`：

```text
# 现行规则：
FAIL the generated layout routes: ... (no clear wire from (-15,-8) to (113,0); move the nodes apart)
# 冲突判定关掉之后：
布局与编码都过了；但真值表不过：32 组输入里有 24 组点亮的不是自己那根线
（input 1 -> 第 15 根，input 2 -> 第 8 根，input 3 -> 第 24 根，input 4 对）
```

两件事因此必须分开看：

1. **布局不是障碍。** 交叉合法之后，"层级各占一列、父线带相邻两个 AND、译码器线带隔一个的 AND"
   这套画法可以直接落笔（现有的候选路由就够，不需要路径点，也不需要动"目标自己那一列"那条通道）。
2. **真值表是障碍，而且它一直被上面那条拒绝信息挡着。** `validate()` 先失败，用例从来没走到真值表
   那一步；把冲突判定关掉之后第一次真跑，32 组里 24 组错。所以"离线模型对全部 32 个输入组合给出
   正确的 one-hot 真值表"这句话**目前没有证据**。第一处可疑点在生成器的线号顺序：线号里**最新**的
   输入位落在最低位（`line = 2*previous + bit`），而输出脚按 `laneY(line, 32)` 从小到大排，两者差
   一个位序；译码器 0 线/1 线的极性也还没被真机钉过。这一条要单独定位。

**下一步（顺序不能换）**：

| 顺序 | 做什么 | 为什么 |
|---|---|---|
| 1 | 真机探针，把三种情形各造一次证据：① 两根不同网络的线**交叉**；② 一根线**结束**在另一根线中间（T 形）；③ 两根共线**重叠**但只有各自端点分别落在两条线上 | `routesClash` 要按真实规则重写：①按玩家说明合法；②在编辑器里会把那根线分裂、变成同一个网络，所以必须继续拒；③没有共享端点，但游戏会不会按"重叠即同网"处理**没人量过**。规则改错的代价是 Mod 的电路静默变成另一个电路 |
| 2 | 按探针结果重写 `routesClash`（只保留"共享端点 / 端点落在别人线上"这类），并在 `tests/component-netlist.cpp` 补正反用例 | 放宽必须仍然拦得住真错 |
| 3 | 定位并修掉生成器的位序/极性，让 `tests/component-decoder.cpp` 离线跑绿 | 布局不再是障碍，真值表才是 |
| 4 | 再接 `component-composite` 的真机门禁（导入 + 游戏重算门数 + 32 组合真值表） | 第 3 步绿了才有意义 |

顺带记一笔：本次还试过一版"复用原版宽译码器"的 8 个零件电路（低位三位拼成 w3 → 四份
`builtin/3 Bit Decoder`，每份的 enable 由一份 `builtin/2 Bit Decoder` 给）。它当时失败的原因和
65 零件那版一样是过严的冲突判定；既然现行网表已经能落笔，这一版先不做，除非以后要省零件。

几何上仍要注意：builder 只查脚点和别的线，不查"线从元件身上穿过"，这类形状要单独确认。

**2026-09-29 当日收口**：第 1 步没有走探针那条路——用户直接给了规则（导线只有两个端点，交叉处
没有端点即不同网络），于是第 2 步按这条规则重写了 `routesClash`：只保留"端点落在别的网络上"
（等于放一个端点把两根线接成一个网络）与"共线重叠"两类拒绝，交叉放行；
`tests/component-netlist.cpp` 新增一节 `routing rule`，把交叉合法、端点落在别人线上被拒、共线重叠
被拒、同源扇出两件事都允许逐条钉住。第 3 步一并做了：生成器把树的**根从 A0 换成 A4**（线号因此
等于输入字、A0 为最低位），真值表从"32 组错 24 组"变成全过，并改用宽通道 `pin_lane` 16。
`tests/component-decoder.cpp` 由此进 `build.ps1`，离线全绿。

**仍未做**：第 1 步里"端点落在别人线上"和"共线重叠"这两条目前只是按规则推定，没有真机证据；
第 4 步的真机门禁（导入 + 游戏重算门数 + 32 组合的关卡真值表）也还没跑——元件形态走的是
`tests/decoder-probe.cpp`（借 `tc.component.compose` V2 注册成组合元件，`build.ps1` 打成
`dist/dev.decoder-probe.mod`），注册成功与否看 `loader.log` 里 `decoder:` 那几行。

- **有状态零件的输入位置进 builder**：目录只报"没声明输入脚"，实测偏移（Delay Line `(-3,0)`→
  `(3,0)`，research §11.2）还没进契约，所以有状态零件目前只能当源用，或者由调用者自己填 `sinks`。
  这是最自然的下一件——做完就能拼延迟线反馈这类组合。
- **字宽内部节点**：节点有 `bits` 字段但没有宽度校验（位宽不匹配靠游戏自己报错）。
- **声明代价与重算门数的关系**：现在刻意不覆盖重算值；如果以后要允许 Mod 声明"展示用"的门数，
  得先决定面板/分数读哪一个。
- **内层属于别的 Mod、且那个 Mod 后来被卸载**：外层定义已经在游戏里，游戏怎么处理缺失的内层。
- **每个 kind 的能力位矩阵**（LOGIC/STATE/RENDER/INPUT/EDITOR/SERIALIZE）：只有找到"新 id 走原版
  分发"的入口才有意义；当前结论是没有。
