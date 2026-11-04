# 上限与约束速查

## 包与部署

| 项目 | 上限 / 规则 |
|---|---|
| 单个 `.mod` | 128 MiB、4096 个条目 |
| 单文件解压 | 64 MiB |
| 解压总量 | 256 MiB |
| Mod id | 1–80 字符，`[a-z0-9._-]`；同目录重复 id 不可启用 |
| 路径 | `/` 分隔；禁止绝对路径、`..`、Windows 保留名、备用数据流、重解析链接 |
| 大小写 | 路径冲突不区分大小写 |
| 可部署位置 | 仅资源目录（`asset/`、`campaign/`、`translations/` 等）；禁止 EXE、DLL、存档、加载器目录 |

## Mod 配置、迁移与设置页（`tc.mod.storage` V1/V2、`tc.mod.settings` V1）

| 项目 | 上限 / 规则 |
|---|---|
| 所有权 | 由服务上下文绑定调用 Mod；调用方不传 Mod id，也不能访问别的 Mod |
| scope | `GLOBAL` 跟安装走；`PROFILE` 位于本次运行选择的独立存档内，复制存档时一起复制 |
| key | 1–64 字节，只允许小写 `a-z 0-9 . _ -`；单一名称，不是路径 |
| 单值 | 最多 1 MiB；零字节值有效，与不存在不同 |
| 每 scope | 每 Mod 最多 256 个 key、合计 8 MiB |
| 写入 | 整个 blob 原子替换；宿主内部串行化文件操作 |
| 读取 | 支持 `out=NULL, capacity=0` 先问长度；缓冲不足返回 `ERR_SIZE` 并报告所需长度 |
| schema | V1/raw 值为版本 0；V2 的版本和 payload 在同一 envelope 原子替换。回调失败/越界/竞态保留旧值；不允许降级 |
| 设置字段 | 每 Mod 64；id 48 字节；bool/int32/finite float/enum(16 项)/text(255 字节) |
| 设置页面 | 至少一张字段表时主页出现一次统一入口；按 GLOBAL / PROFILE 分组；每行默认值与 Reset |

## 宿主与 Hook

| 项目 | 规则 |
|---|---|
| ABI 版本 | `TC_MOD_API_VERSION` = 1；`host->api_version` 不符必须拒绝 |
| 结构扩展 | 先比较 `host->size` / `out->size` 再访问尾追加字段 |
| Hook 目标 | EXE 函数符号（`resolve_symbol`）或原引擎可执行导出（`engine_proc`）；任意地址/数据页拒绝 |
| 同一目标 | 原始的 `create_hook` 只能有一个；第二个直接失败 |
| 钩子链 | 目录内的钩子点（`sim.do`、`level.load`）由加载器拥有唯一 detour，多个 Mod 各加一个链节；每个点最多 32 个链节，顺序为 优先级 → Mod id → 注册顺序 |
| 钩子点保留 | 对链上目标调用 `create_hook` 会被拒绝（提示改用 `register_hook_chain`） |
| 注册时机 | 仅 `tc_mod_load` 期间；不支持热卸载 |
| Hook 状态 | 创建时禁用，入口成功后宿主统一启用；失败插件自动移除 Hook |
| 线程 | `on_frame` 在主/渲染线程；Hook 回调在各自原调用线程；逻辑回调在仿真线程 |
| 棋盘对象读取 | 句柄只在其签发帧内有效；元件序列 ≤ 1e6、导线序列 ≤ 4e6，超出即视为不可读；记录必须仍落在当前序列的元素位置上，否则 `STALE` |
| 元件序列占位项 | 每条板都有一条 kind 为 0 的全零记录，它不是元件，Mod 按 `kind == 0` 跳过 |
| 元件计数 | `component_count` 是序列长度；撤销会把被删的槽留成 kind 0 墓碑，**活元件数要自己跳过 kind 0** |
| 撤销粒度 | 一次 `BOARD_UNDO` 只回退一步；两步事务需要两次 undo（实测），Redo 对称 |
| 元件引脚读取 | 先零容量取数量再分配；顺序固定为输入在前；内置 kind 只接受启动时从 `PROTOTYPES` 枚举出的集合，未知 kind 返回 `UNAVAILABLE`；`AUTO_SIZE` 只置 flag |
| 仿真读数 | 只读；偏移必须落在 10,240,000 字节的状态缓冲内（越界 `RANGE`），`bits` 1–64；状态在仿真步进结束时才写好，周期刚变化的那一帧可能仍是上一周期的值 |
| 编辑命令（V2） | `PLACE_COMPONENT` 需要当前 Board 句柄；自定义原型时 `kind` 必须是 `0x4e`，内置时 `kind` 非零；坐标须落在 16 位域内，`rotation` ≤ 255；助手返回 false 记为 `FAILED/STATE`（**目标格已被占用**就是其中一种：game 自己的放置会拒绝重叠，实测同一格放第二个元件返回 `-6`，换一格即可——这也是"放元件失败"最常见的真机原因） |
| 删除命令（V2） | `DELETE_COMPONENT` 用 `(argument = 实例 id, custom_prototype_id = 类型)` 定位；类型归属以元件目录的 `owner_mod` 为准，目录记在**另一个** Mod 名下的类型入队前即 `ARGUMENT` 拒绝（`request` 保持 0），目录里没有记录的类型（Mod 自己用 `tc.component.model` 导入的电路）不做归属判断，与 `DUPLICATE_COMPONENT` 一致；目标不在棋盘时 `FAILED/NOT_FOUND`；走游戏自己的撤销感知包装，所以一条 `BOARD_UNDO` 会把元件（连同其导线）放回，序列里留下 kind 0 墓碑 |
| 连接判定（V6） | 按几何匹配：导线端点 == 元件坐标 + 引脚偏移；只比较 ±32 格窗口内的元件，窗口内先命中的引脚即结果；两端各自独立，悬空端不带 `HAS_COMPONENT` |

## 原生逻辑回调

| 项目 | 上限 / 规则 |
|---|---|
| 输入脚 | V1 ≤8；V2/V3 ≤32 |
| 输出脚 | V1 ≤8；V2/V3 ≤32 |
| 单脚位宽 | 1–64 位 |
| 总输入宽度 | V1 ≤512 位；V2/V3 ≤2048 位（超过 128 位时按 128 位分块暂存） |
| 内部门 | 每个输出脚一个可识别节点：`0x03`–`0x0b`、`0x12`–`0x1e`、`0x2a` |
| 回调输入元组 | 第一个输出门的操作数列表，顺序 = `inputs[]` 顺序 |
| 仿真状态 | V1 每实例 `state[8]`；V2/V3 为定义声明的 `state_words` |
| 三态回调 ABI | `TCLogicIOV3` 已接通注册与生成代码桥，值为 `{value, drive_mask}`（0/1/Z，无 X）；当前多位引脚只能整脚驱动或整脚 Z |
| TC-Verilog | 可综合子集；端口/信号 1～64 位，默认最多 256 KiB 源码、65536 token、65536 个每实例状态 word；多 `module` 与**模块实例化**（编译期展开、具名/位置/空连接、常量位选连接、`#(.P(v))` 具名参数覆盖）、**层级名字只读引用**（`u0.sig` / `u0[1].sig`）、**generate 的 `if`/`case` 形式**、`parameter`/`localparam`、拼接/复制、位选/部分选择（含 `[base +: width]`）、`case`/`casez`/`casex`、多事件边沿敏感表（含 `negedge` 异步复位）、**组合式块**（`always_comb` / `always @(*)` / 电平敏感表，用 `=`；无环块拓扑执行一次、环才迭代；完整赋值的无反馈变量使用临时 scratch）、逐输出反向执行切片、纯 scratch 块按读取叶子跨输出复用、相同依赖掩码的输出分组、tick-only 时序提交、常量边界的 `for`/`generate` 展开（上限 4096 轮，每轮/每生成块独立作用域）与 `signed`/`'s` 字面量/`$signed`/`>>>` 的有符号算术已支持；暂不支持 `inout`、`always_latch`、层级名字赋值、函数/任务、`initial` 与延迟 |
| 实例数 | 每定义不限，但字宽输出用到的状态槽最多 32 个 token（真机实测：42 实例链上 token 1–33 正确，token 33 起没有自己的槽，消费者读到 0，见 `docs/PLAN-float-components.md` §4.2 与 `tests/float-boundary-playtest.ps1`） |
| 状态槽区间 | `0x9a0000` 起，每 token 2048 字节（32 输出脚 × 64 位，字宽输出按位回读） |
| 外调参数 | 每次最多 4 个参数；≤128 位走 `invoke(token, cycle, lo, hi)`，更宽输入先走多次 `stage(token, selector, lo, hi)` |
| 外调返回值 | 只有按 1 位消费可靠；字宽输出必须走状态槽 |
| 时钟阶段 | `CYCLE` / `REFRESH` / `RESET` 三种；`REFRESH` 不提交 `state` |

## 元件类型目录（`tc.component.registry`）

| 项目 | 上限 / 规则 |
|---|---|
| 每会话类型数 | 256（超过后 `register_*` 仍然失败，目录拒绝增长） |
| 每方向报告的引脚 | 32（当前定义 API 只允许 8，留出可变长阶段的余量） |
| `type_id` / `name` / `description` / `owner_mod` / `status` 字段 | 79 / 63 / 191 / 79 / 159 字符，超出截断（字段容量就是上限） |
| 引脚名 | 63 字符，超出截断 |
| 拒绝原因 | 每个 id 只保留**第一条**；同一 id 之后注册成功会替换整条记录 |
| 被拒绝的 Mod | 它的类型随插件一起从目录里删除 |
| 说明文字 | 定义里的 `description` **默认不写进游戏原型**（游戏会把它印在底部元件面板的顶部，而那正是属性行的位置）；V2 表尾部的 `flags` 设 `TC_COMPONENT_TYPE_SHOW_DESCRIPTION`（1）才显示。文字始终保留在加载器自己的目录里（Mods 页用）。旧的 V1 表没有 flags 字段，保持发布 |

## 原版类型目录（`tc.component.compose` V1）

| 项目 | 上限 / 规则 |
|---|---|
| 条目数 | 本构建 **125** 个 kind（引导时枚举整个原型表；表里有多少就报多少，含游戏没命名的槽位） |
| 拼写 | `builtin/<名字>`；同一构建里重名按 **kind 升序**编号 `#2`、`#3`…；没名字的槽位用 `builtin/0x<kind>`（本构建 17 个名字重复、21 个槽位无名） |
| 查找 | `get`（按 kind 升序的位置）、`find(kind)`、`find_by_type_id`：先精确，再对**无编号**拼写做 ASCII 大小写折叠；编号拼写永远精确 |
| 字符串字段 | `type_id` / `name` / `description` 容量 79 / 63 / 191 字符，超出截断（与元件目录同一套字段容量） |
| 每方向报告引脚 | 32（`TC_BUILTIN_PIN_LIMIT`；本构建实际最多 8） |
| 引脚宽度 | `word_size_raw` 原样报出。1–64 时 `bits` 就是它；`0x7fff…` 自动标记置 `AUTO_WIDTH`；本构建两个 Splitter 用的 `0x7fff…fe`／`…fd` 未核实语义，置 `RAW_WIDTH` 且 `bits=0`——**不要按 `bits` 猜** |
| 输入脚 | 原型一个输入脚都不声明的（本构建 41 个 kind）置 `TC_BUILTIN_FLAG_NO_DECLARED_INPUTS`。有状态族与纯源都在这一组；有状态元件的输入位置仍以 [research/component-pipeline.md](../research/component-pipeline.md) §11.2 的实测为准，尚未进契约 |
| shape | 原型带 shape 的（本构建 104 个）置 `TC_BUILTIN_FLAG_HAS_SHAPE` |
| 不报告 | 代价（门数/延迟由游戏自己的打分器从内部电路算出；原型里那两个字段本构建全是 0）与能力位（LOGIC/STATE/RENDER/ALIASABLE——别名那条路的研究项） |
| 可用性 | 引导时读不到原型表时**不发布**这张表（`query_service` 返回 `UNAVAILABLE`），而不是发一张空表 |

### 内部网表 builder（同表的 V2）

| 项目 | 上限 / 规则 |
|---|---|
| 外层引脚 | 每方向 **≤32**（游戏导入器实测的上限，与 `docs/research/custom-component-pins.md` 的测量一致；真机 `component-composite` 注册了一个 2 入 / **32 出**的组合类型并读回 `in=2 out=32`）；至少一个方向非空；每脚 1–64 位 |
| 引脚通道宽度（V2 `pin_lane`） | 1–16 板面格，默认 2（历史形状 -18 / +13，既有的 3 入/1 出等类型不变）。通道越宽，输入列越靠左、输出列越靠右，中间空出来就是内部电路的画布；声明式 API 早有同名旋钮且宽通道的导线位置已验证。真机：2 入/32 出 + 通道 16 的类型，游戏读回两列引脚相距 **31 格**（`in0=(-15,0)`、`out0=(16,-120+…)`） |
| 内部节点 | ≤4096；每个节点至少一个引脚；`bits` 1–64 |
| 零件来源 | `builtin/<拼写>`，或已注册类型的 `type_id`/`0x<id>`（嵌套；内层必须先注册） |
| 未命名内部输入 | 任何内部 sink 没被驱动都**拒绝**（外层输出脚也算 sink）——游戏会把它当 0 读，静默出错比报错糟 |
| 多驱动 | 同一个 sink 被两条线驱动 → 拒绝；一个 source 扇出多条线 → 允许 |
| 组合环 | 经过"不声明输入脚"的零件（Delay Line/Register/Counter…）的环**合法**；没有这类零件参与的环拒绝 |
| 布线 | builder 选路径；**不同网络的线可以交叉**（规则：一根导线只有两个端点，交叉处没有端点就是两个网络——见 [HANDOFF-component-m4.md](../HANDOFF-component-m4.md) §5.1 与 `tests/component-netlist.cpp` 的 `routing rule` 一节）；**拒绝**会穿过别的引脚、**穿过元件本体**（元件的引脚包围盒再放一格净空；只有接到自己那个引脚的那一段允许进入这块区域）、**端点落在别的网络上**（等于放一个端点把两根线接成一个网络）、或共线重叠的布局（`status()` 给出冲突点）；**同一网络**（同一个源脚扇出）的线允许相接、可以画成一条干线加分支（离线：`tests/component-netlist.cpp` 的 bus 用例，一个源脚驱动六个 sink）。节点位置由调用者负责 |
| 代价 | 声明里的 `gate_cost` **不写进原型**：游戏按内部电路重算（真机 2 个 AND 节点 → `gates=2`）。`delay` 原样写入，必须声明真实关键路径 |
| 提交后缺什么 | 没有 `tc.component.instances` 绑定、没有 `tc.component.storage` 配置、没有生命周期回调（这些属于有回调的类型） |
| `clone_metadata` | 只复制 `NAME`/`DESCRIPTION`/`COST`；引脚不能事后改（会让实例失效）；**图标不能复制**（游戏对自定义原型忽略 shape SVG，用 V5 `set_picture`） |
| 完整别名 | 本构建**不可用**：`alias()` 恒返回 `UNSUPPORTED`，目录的 `TC_BUILTIN_FLAG_ALIASABLE` 从不置位（游戏按 kind 字节分发，Mod 实例恒为 `0x4e`） |

## 元件定义（`circuit.data`）

使用 `TCNativeComponent::registerWith` 时，定义和连接由加载器生成，作者无需处理以下
序列化规则。声明式接口支持每方向 0–8 脚（0 表示纯源或纯汇；两个方向同时为 0 表示**装饰元件**，
加载器为它补一个不接线的驱动节点，元件仍进平坦序列、仍有实例句柄，但既不读也不写信号——2026-09-23
复测确认，见 [research/text-component.md](../research/text-component.md) 与
[research/custom-component-pins.md](../research/custom-component-pins.md)），输入／输出顺序就是声明顺序；
手写定义走原有 `register_logic` 时仍适用下面的一层占位门规则。

**2026-10-05 的当前边界（取舍，含 2026-09-22 外调实测）**：每脚 1–64 位（与游戏原版一致，**不做多字 >64 位信号**）；
**总输入位宽 V2/V3 ≤2048 位**——生成代码里的单次外调仍只可靠传 4 个参数（扩到 6 个时第 5、6 个
参数收到垃圾，实测），因此不加宽调用；加载器先用若干四参数调用暂存 128 位分块，再以带周期和
分块数的四参数调用触发回调。每脚仍为 1–64 位，最多 32 个输入脚，所以自然上限是 32×64=2048 位。

| 项目 | 规则 |
|---|---|
| 多脚引脚几何 | 输入左列、输出右列，间距 8（例如 `(-18,-10) (-18,-2) (-18,6)`） |
| v14 组件尾部 | `bool_a`、`value_i64_a(-1)`、`value_i64_b(0)`、`bool_b` 顺序固定 |
| 缓存统计 | 定义头两个 i64 = `(门数, 延迟)`；游戏解析时会重算门数、保留延迟；加载器会用登记元件在 `tc_mod_load` 里写下的声明值覆盖两者 |
| 引脚偏移 | 由游戏计算，以注册日志实测值为准 |
| 内部电路（登记回调的元件） | 一层占位门：可识别门数 = 输出脚数，且第一个门（最低节点序号）的操作数就是全部输入脚。中间层暂缓，计划以后作为独立 Mod 提供。这份内壳同时就是**元件工坊看到的图**——加载器在注册时把同一份定义写进 `<存档>/schematics/foundry/<名字>/circuit.data`（2026-10-05 起 V1/V2/V3 与运行期注册都覆盖），所以选中元件点"在元件工坊中编辑"能看到引脚与接线；用 `setFoundryButton(..., false)` 隐藏了按钮的类型要设 `TC_FOUNDRY_SHOW_MOD_TYPES=1` |

**沙盒仿真器另外两条**（与加载器生成的定义无关，是 simcore 成表时的边界，见
[research/custom-components.md](../research/custom-components.md)）：几何表每侧最多 **64** 个脚
（自定义元件的接口是作者画的，玩家的 `Symphony/DEC` 是 1 进 7 出）；自定义定义的展平递归
深度上限 **8** 层。

## 元件实例与存储

| 项目 | 上限 / 规则 |
|---|---|
| 实例引用 | `custom_id + instance_id + generation`；删除、离开棋盘或槽位复用后旧句柄 `STALE` |
| 实例的服务端版本 | `tc.component.instances` V1 为枚举/校验/信息/状态/单实例重置；**V2 追加 `top_level_id`**：游戏选中集与顶层遍历认的那个 id（直接放在棋盘上的元件就是它自己，装在别的自定义元件内部时是外层元件；只在编译遍历时记录，调用不读棋盘） |
| 生命周期回调 | `TCComponentLifecycleV1`（按 `size` 逐段生效）：`on_create` / `on_destroy`（2026-09-22）、`on_config_changed`、`on_clone`、`on_load`、`on_save`（2026-09-22）、`on_move` / `on_rotate` / `on_resize` / `on_compile` / `on_unbind`（2026-09-28，追加在结构末尾）。放置三个回调读 `TCLogicIOV2` 的放置尾巴（`cell_x/cell_y/rotation/half_width/half_height`，同样按 `size` 判定） |
| 放置回调的真机状态 | `on_resize`（逐实例 footprint 驱动）与 `on_compile`、`on_unbind` 有真机断言（`pin-shape-wide9`）；`on_move` / `on_rotate` 的**派发**由离线用例 `native-component` 覆盖，但它们依赖"棋盘记录真的变了"——本构建不响应合成鼠标的棋盘拖动（见 [research/component-hitbox-path.md](../research/component-hitbox-path.md)），所以自动真机门禁仍未关，列为待办 |
| 状态与销毁的区别 | `on_destroy` = 元件被玩家删除（该次编译里没有它）；`on_unbind` = 棋盘整个消失（离开关卡/切场景/加载器退出），元件还在原理图里 |
| `top_level_id` 失败语义 | 句柄过期 `ERR_STALE`、只认 V1 的加载器 `ERR_UNAVAILABLE`；两种失败都保证出参为 0，调用方不会拿到半写答案 |
| V2 仿真状态 | `state_words` 由定义决定；RESET 清零后运行回调 |
| 配置 blob | 每实例最多 65536 字节；注册时复制默认值；RESET 不清配置 |
| 配置写入 | 必须整 blob、schema 与长度完全匹配；宿主在实例锁内原子替换 |
| 可存档配置 | 每实例 **1024 字节** = **132 个表项**（4 头部 + 128 分块）。真机实测：写满后存档→重启回读字节一致；每个满配实例让原理图增加约 **1 KB**（原始 132×16 ≈ 2.1 KB，压缩后） |

浮点元件族的取值直接由上面两条约束推出（`local.float-ops`，见
`docs/PLAN-float-components.md` §6.4.1）：因为**每脚位宽在注册时按类型冻结**，一个格式就是
一个元件类型，所以

| 项目 | 规则 |
|---|---|
| FP16 元件 | 字脚 16 位 |
| FP32 元件 | 字脚 32 位 |
| FP64 元件 | 字脚 64 位；二元运算 2×64 = 128 位，仍走单次调用快路径 |
| FP64 Fused Multiply Add | 3×64 = 192 位，加载器现已支持；需要元件包注册该类型后才会出现在目录中 |
| FP16/FP64 常量配置 | binary64 需要 8 字节位模式，因此新宽度用 28 字节的 `ConstantConfigWide`（FP32 的 16 字节旧布局一个字节不变） |
| 记录键空间 | 保留键为 `TCM3` 魔数 + 字段号；不认识的表项原样保留，最多 `0x1000` 个分块键，宿主只写自己那几项 |
| 记录布局 | 由构建画像的 `layout` 段给出；画像未命中时 `HAS_PERSISTENCE` 为空，配置退回内存层，不猜偏移 |
| 记录写入线程 | 只在游戏线程；记录本身按 `(custom_id, instance_id)` 每次重新在当前棋盘里解析，不保存记录或表指针 |
| 记录读取 | 只在实例绑定时与 storage 服务调用时；记录损坏/换定义/换 schema 一律拒绝并保留原字节 |
| 记录迁移 | 定义声明的迁移回调在**游戏线程 + 实例绑定时**调用一次；只有 `MIGRATE_OK` 才写回记录，`KEEP`/`REJECT`/未知返回码都保留原字节 |
| 缺失 Mod 救援 store | 每关卡一个文件（`missing-mods/<关卡>.bin`）：记录数 ×（custom id、实例 id、坐标、旋转、tail 键值）。进入条件只有"元件被游戏丢弃且主人没注册"；注入成功后条目立即移除，不会无限增长 |
| 配置撤销栈 | 每块板最多 64 步（LIFO，新写入清空重做）；实例释放或换板即丢弃相关步骤。只有存在未消费步骤时宿主才接管游戏 undo/redo，栈空时原样交给游戏。一次 `write_config` 一步；`begin_edit`/`commit_edit` 之间的多次写入合并成一步 |
| M3 剩余边界 | 尚无"缺 Mod 期间画面上可见的占位元件"（并入 M5）、把配置提交与棋盘编辑合成一组事务、复制（`on_clone`） |

## 元件几何（`tc.component.geometry` V1/V2）

| 项目 | 上限 / 规则 |
|---|---|
| 声明时机 | 仅游戏线程上的 `tc_mod_load`；必须先成功注册该类型 |
| 所有权 | 只能修改调用 Mod 自己登记的 `custom_id` |
| 形状 | V1 为一个以元件锚点为中心的 footprint 矩形；同时用于放置/占位和整框鼠标拖动（含没有游戏像素的空白区域） |
| 半宽 / 半高 | 有限浮点数，至少 `0.5`，向外取整到整数格；完整边长不超过 32767 格 |
| 旋转回读 | 0°/180° 原宽高；90°/270° 交换宽高 |
| 未声明类型 | 保留游戏原型的默认几何，旧 Mod 行为不变 |
| V2 逐实例交互矩形 | 可在游戏线程运行期更新；只参与原生点命中/选择/拖动/删除，不改变类型级占位和放置碰撞；实例句柄过期返回 `ERR_STALE` |
| 尚未交付 | 运行期改变放置/占位 footprint、非矩形命中形状 |

## 元件绘制（`tc.component.render` V1）

| 项目 | 上限 / 规则 |
|---|---|
| 登记时机 | 仅游戏线程上的 `tc_mod_load`；只能登记调用 Mod 自己的类型 |
| 回调时机 | 当前棋盘的渲染帧；离开棋盘或实例消失后不再调用 |
| 图元配额 | 每实例每帧 4096 条；超出返回 `ERR_CAPACITY` |
| 文本 | 单次 UTF-8 最长 4096 字节；V1 使用宿主默认字体 |
| 指针寿命 | `frame`、`draw`、config 与 draw context 只在本次回调内有效 |
| 坐标 | 屏幕像素；frame 的两个轴向已包含 DPI、相机缩放和元件四向旋转 |
| 裁剪 | `clip_min/max` 为半开屏幕矩形；宿主对所有 V1 图元实际应用该矩形，回调不能越界绘制 |
| 关闭默认绘制（V2） | 按类型生效，只对带实例 id 的棋盘实例；元件栏/选择器预览保持画图；不改 footprint、命中、引脚、选中/删除和本 Mod 的回调；一次调用只影响本 Mod 自己登记的类型 |
| 元件图片（V5） | `set_picture(id, png_path)`：绝对路径、单个 PNG（加载器按 64 MiB 单文件上限读取并复制到自己的目录），`NULL` 或文件消失即回退游戏自己的渲染；覆盖元件栏卡片与抽屉预览两个面，覆盖不了放置幽灵（该面游戏不画本体，见 research/component-icons.md §5）；只对调用 Mod 自己的类型生效 |
| 回调自绘图片（V7 + 绘制表 V3） | `load_image(key, png_path)`：key 为 `a-z 0-9 . _ -`、≤64 字节；绝对路径的 PNG 只读一次；**每个 Mod 最多 16 张、解码后合计 ≤16 MiB、单文件 ≤64 MiB**；纹理由加载器（`ui_texture::Manager`，owner = 该 Mod）持有，插件卸载/停用时随其它纹理一起回收；重载同一 key **保留 handle、revision +1**，旧的 GL 对象延后一帧释放；`release_image(key)` 单个回收、`key=NULL` 全清；绘制走 `TCComponentRenderDrawV3::image`（四角 + 四组 UV + tint，计入每实例每帧 4096 条配额，按 frame 的裁剪框裁剪）；句柄按 Mod 隔离，画别人的 handle 得到 `ERR_IMAGE`；`ERR_IMAGE` = 键/路径/解码/尺寸/配额问题 |
| 插件对渲染句柄的责任 | 插件永远拿不到 renderer id 或 GL 对象，只能拿 handle；因此不存在"持有过期 id"的问题。本构建的 GL 上下文只随窗口创建一次（纹理管理器按 context 比较），**没有设备重建路径**，加载器保留解码后的像素以便将来需要时重传——该路径在当前构建中没有不可触发，也不作为承诺 |
| 字体资源（V8） | `load_font(key, ttf_path, native_size, ranges, range_count, out)`：绝对路径、≤16 MiB、无重解析点，按 Mod 隔离，每 Mod 最多 **8** 个面；`native_size` 4–512；`ranges` 最多 **16** 段、每段是 BMP 内的闭区间（`first ≥ 1`、`first ≤ last ≤ 0xFFFF`）。宿主用游戏自己的 ImGui 建 atlas、上传纹理、保留解码像素并在设备世代变化时重传，插件只拿 `font` 句柄（`revision` 从 1 起） |
| 字体区间必须是 32 位 | 本构建的 ImGui 用 **32 位 `ImWchar`**（它自己的默认区间表是 `20 00 00 00 / ff 00 00 00 / 0`）。宿主把 SDK 的 `TCComponentRenderFontRangeV1`（`uint32_t` 对）直接交给 atlas；按 16 位编码会被读成另一组区间，字面就没有可用字形（2026-09-28 实测：那种面画出来的文字没有顶点） |
| 字体生命周期 | 同一 key 再加载**不释放** = 替换：句柄不变、`revision +1`，旧 atlas 立即销毁；`release_font(key)` 或 `key=NULL` 释放（后者清空该 Mod 的全部字体），释放后再加载是全新句柄、`revision` 重新从 1 起；插件被拒/停用时随其它资源一起回收 |
| 字体绘制 | 在**元件 render 回调**的绘制表 V4 上：`text_font(font, x, y, size, color, style, utf8)` 与 `measure_font(font, size, style, utf8, &w, &h)`；`font=0` 表示宿主默认面，`style` 支持 `BOLD`（宿主叠画一次）与 `ITALIC`（按水平带切条偏移合成）；`measure_font` 报出与面一致的度量 |
| 字体尚未关闭的边界 | 由宿主加载的自定义面，**绘制像素**仍未验证：同一位置 `text_font(0,…)` 有 102 个像素、`text_font(句柄,…)` 为 0，而该 atlas 纹理本身作为图片四边形能画出内容（151 像素）、`measure_font` 也按面给出不同度量。下一步是查清这个构建里字体 atlas 的纹理 id 到底由谁绑定（`ImFontAtlas_SetTexID` 之外是否还有后端路径）。见 [verification.md](../verification.md) |
| 当前边界（其余） | 元件栏分类仍未服务化（见 §21 M4/M5） |
| 设备世代（V8） | `set_device_callback` + `device_generation`：纹理管理器保留解码像素，GL 上下文变化时重传并 +1 世代，宿主角逐 Mod 通知（句柄不变）。本构建的上下文只随窗口创建一次，**没有设备重建路径**，因此这条当前不会被触发，也不作为承诺 |

## 元件属性行（`tc.component.editor` V1–V4）

| 项目 | 上限 / 规则 |
|---|---|
| 声明时机 | 仅游戏线程上的 `tc_mod_load`；必须先注册该类型 |
| 所有权 | 只能声明调用 Mod 自己的 `custom_id` |
| 每类型字段数 | 32（超出 `ERR_LIMIT`） |
| 字段落点 | `offset + bytes` 必须落在该类型注册的 `config_size` 之内，否则整表拒绝 |
| 整数 | `INT` 1/2/4 字节；`UINT` 1/2/4 字节且**有效位 ≤31**（V1 控件是有符号十进制框） |
| 十六进制 | `UINT` + `TC_COMPONENT_FIELD_HEX`（V2）：1/2/4/8 字节、有效位 ≤64，以 `0x…` 文本编辑；解析不了的输入拒绝并保留旧字节 |
| 文本 | 2–64 字节，UTF-8，末字节为终止符；超长提交按截断处理并写日志 |
| 浮点 | 4 字节（binary32），可选 `min_float`/`max_float` 钳位 |
| 十进制浮点（V2，`TC_COMPONENT_FIELD_FLOAT_WIDTH`） | **2/4/8** 字节 = binary16/binary32/binary64；行里显示"能解析回原位模式的最短十进制"，输入按本宽度正确舍入（binary16 经 double 中转仍精确）；解析不了的文本拒绝并保留旧字节；`min_float`/`max_float` 钳位 |
| 枚举 | 选项数必须能装进声明的字节宽度；点一行前进一格并回绕 |
| 字节序 | 小端；配置是字节布局，不按 C 结构对齐 |
| 提交 | 一次提交 = 一步撤销，并触发 `on_config_changed`；与 `tc.component.storage` 共用每板 64 步的撤销栈 |
| 编辑状态 | 每实例保留一份工作副本与文本缓冲；实例释放或换板后由下一次绘制清理，表最多缓存 64 个实例 |
| 只读行 | `TC_COMPONENT_FIELD_READ_ONLY` 画值不编辑；`TC_COMPONENT_FIELD_HEX` 在 V1 只读 |
| 诊断 | `TC_EDITOR_TRACE=1` 时每秒打印各行的屏幕矩形 |
| 布局标志（V2） | `FULL_ROW`（控件占整行）、`INLINE`（与上一字段共用一行，最多两个）、`SEPARATOR`（本行前画分隔线）、`RADIO`（ENUM 铺一行单选）。第一行不能 INLINE，`FULL_ROW`/`RADIO` 之后也不能 INLINE；未知 flag 位拒绝 |
| 滚动（V2） | 行窗口按内容高度夹进屏幕，再用尺寸约束封顶在屏幕内，装不下的部分由 ImGui 滚动；`TC_EDITOR_TRACE` 的 `window` 行给出 `rows`/`scrollmax` |
| 下拉（V3，`TC_COMPONENT_FIELD_DROPDOWN`） | 仅 ENUM；与 `RADIO` 互斥；解析不出选项时整表拒绝。ImGui 的弹窗会先出现在指针处、再翻到字段上方，宿主会把弹窗与每个选项的矩形写进日志（`popup`/`popup item`），供真机用例精确点击 |
| 颜色（V3，`TC_COMPONENT_FIELD_COLOR_RGBA8`） | 4 字节小端 `0xAABBGGRR`，用 ImGui 自己的颜色控件；一次拖动 = 一步撤销（宿主开事务、松手提交） |
| 自定义行（V3，`TC_COMPONENT_FIELD_CUSTOM`） | `offset`/`bytes` 可为 0；宿主给一块裁剪画布 + `TCComponentRenderDrawV1` 绘制表 + 交互事件；`custom_height` ≤4096 px。右下的缩放抓手把行高做成 14 px 起，并把位置夹进窗口内（否则行贴底时抓手会被窗口裁掉，实测点不到）；抓手与前一个画布 item 重叠，宿主用 `igSetNextItemAllowOverlap` 让抓手拿到点击 |
| 行内校验（V3，`TCComponentEditorValidateV1`） | 提交前把**拟提交的整份配置**交给 Mod：`OK` 提交、`WARNING` 提交并记 `validator warned field …`、`ERROR` 拒绝（配置保持旧字节）并记 `validator refused field …`；提示画在行下面 |
| 编辑事务（V3） | `begin_edit`/`commit_edit`/`abort_edit` 从编辑器服务转发到 `tc.component.storage` V2；缩放抓手自动用它，把整段拖动合成一步撤销 |
| 色块条（V4，`TC_COMPONENT_FIELD_COLOR_PALETTE`） | 仅 `COLOR_RGBA8`：色块用 `choices`/`choice_labels` 声明，每条是 `#RRGGBB`/`#RRGGBBAA`（`#`/`0x` 可省，`#RGB` 展开），**每行最多 16 块**（`TC_COMPONENT_EDITOR_MAX_PALETTE_SWATCHES`）；没有色块、或任何一条不是颜色，整表拒绝。宿主逐块发布矩形（`Component editor: swatch … index=N rect=…`） |
| 混合取色行（V4，`TC_COMPONENT_FIELD_COLOR_HEX`） | 仅 `COLOR_RGBA8`：同一行再加一个文本框，内容是 `0xAABBGGRR`（也接受颜色字面量）；解析不了的输入拒绝、配置保持旧字节、输入框退回实例真正持有的值。与 `HEX` 的 UINT 行同一条契约 |
| 校验标记（V4，`TC_COMPONENT_FIELD_VALIDATION_ICON`） | 任意种类；消息不再占一行文字，而是控件末尾一个小方块（红=ERROR / 琥珀=WARNING），tooltip 与日志里仍是同一条消息。标记用窗口绘制表画，不提交 ImGui 项，所以不改变行高与排版；矩形发布为 `validation icon … rect=…`，hover 时另有 `validation tip … message="…"` |
| 暂存表（V4，表级 `TC_COMPONENT_EDITOR_TABLE_CONFIRM`） | 经 `set_fields_v3()` 声明：整表改成暂存，值落在宿主的副本里，行下多出 Apply / Cancel。**Apply = 一次配置写**（一步撤销、一次 `on_config_changed`），任何一条被改字段的 validator 返回 `ERROR` 都让整批不写；Cancel 重读实例并丢掉暂存。表里若有 `CUSTOM` 行则整表拒绝（那种行自己写配置） |
| 版本位集（V4） | `TC_COMPONENT_FIELD_KNOWN_FLAGS` 是"当前这一版"的别名，V3 入口按 `TC_COMPONENT_FIELD_V3_KNOWN_FLAGS`（位 0–7，逐位写死）校验，所以 V3 的表不会因为加载器长大而接受 V4 的位 |
| 尚未交付 | 取色器弹窗/吸管/按通道编辑这类第三层取色形状、富文本行；以及 §14 点名的棋盘集成（删除/移动/选中集写入/高亮/镜头）——那几项属于棋盘命令，不是编辑器 |
| 行窗口位置 | 以游戏元件抽屉的内容原点为基准（宽 8%）；**整块窗口严格夹在游戏底部面板矩形内**（每帧读游戏窗口的 `pos/size`，留 6 px 边距）：装不下时封顶在面板底边并滚动，只有放得下时才把面板上半留给游戏自己的内容；面板矩形随窗口尺寸每帧重算，窄窗口收缩字段宽度而不是把控件推出面板 |
| 面板收起 | 钩子（抽屉标题行）在收起状态照常运行，宿主按"标题行以下还剩多少"判折叠：低于 34 px 就**完全不画、也不动任何编辑状态**（文本缓冲、滚动位置、窗口矩形都保留）；重新拉起后行回到原位、已填内容不变 |
| 说明文字 | 游戏会把原型的 `description` 印在这个抽屉的顶部，所以 V2 定义默认**不写进原型**（`TC_COMPONENT_TYPE_SHOW_DESCRIPTION` 可显式打开）；V1 表没有 flags 字段，保持原行为 |
| 真机验收 | `tests/component-editor-playtest.ps1`：注册（含说明文字的两种结果）、复选框、文本、单选、十六进制、下拉（含被 validator 拒绝的选项）、双击、右键菜单、缩放抓手、颜色拖动、色块条、混合取色框、校验标记、暂存表的 Apply/Cancel、32 行长表，以及 `-Fold`（折叠期间零行 trace、展开后同一矩形与同一 `buffer`）。每个 rows 用例还断言行窗口与被点击的行落在 `panel=` 矩形内 |

## 声明统计（编译期统计）

| 形状 | 规则 |
|---|---|
| 串联 | 延迟相加 |
| 并联 | 取最长路径 |
| 嵌套 | 只计最外层声明一次 |
| 反馈环 / 多驱动 / 收缩后成环 / 非法数据 / 溢出 | 保留游戏原生统计（写 `Component timing: native timing retained...`） |
| 门数 | 用声明门数替换内部电路的展开计数；「跳过自定义元件」的调用仍跳过 |
| 生效范围 | 仅本次启动初始化时登记的原生元件 |

## 构建环境

| 项目 | 说明 |
|---|---|
| 目标平台 | Windows x64 |
| 工具链 | MinGW-w64，默认 `C:\msys64\ucrt64\bin`，可用环境变量 `TC_MINGW_BIN` 覆盖 |
| 依赖 | 已随源码固定：miniz 3.0.2、nlohmann/json 3.11.3、MinHook 1.3.4；构建不需联网 |
| 运行时库 | 建议 `-static`（或把依赖 DLL 放在入口 DLL 同目录） |

## 已知风险

- 字宽输出的状态槽区间是固定地址；超大型板子若把仿真状态用到该区域，理论上可能冲突（尚未遇到）。
- 原生插件与游戏同权限，加载器无法隔离访问违规。
- 兼容性按二进制哈希严格限制：只保证 2.1.334 指定构建。
