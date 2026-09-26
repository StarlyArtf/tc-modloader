# 接手：元件接口 M3（存储、迁移和编辑事务）

更新时间：2026-09-22

计划出处：[PLAN-custom-components.md](PLAN-custom-components.md) §9 与 §16 阶段 3。

> 状态：**M3 已收口**（2026-09-22，共五刀）。`tc.component.storage` 的配置写进元件记录自己拥有的 64 位键值表，由
> 游戏随原理图存档，并在下一次绑定时自动回读；写入走游戏自己的表赋值函数，记录布局由构建画像
> 控制，画像未命中时整条持久化路径不可用而退回内存语义；定义可以声明迁移回调把老 schema 的记录
> 转成新 schema，只有明确成功才升级记录。真机证据见 `tests/component-storage-playtest.ps1`
> （`persist` / `migrate` / `reject` 三种模式）与 `tests/component-persistence-playtest.ps1 -Mode insert`。
> **仍未做**：缺失 Mod 占位、复制（`on_clone`）与 Undo/Redo 快照。

## 第三刀交付（2026-09-22）

1. `TCComponentTypeDefinitionV2` 末尾追加 `config_migration_version` / `migrate_config` /
   `migration_user`（结构体 136 → 160 字节，全部在尾部）。注册仍只要求首发前缀：没声明迁移的
   定义得到保守行为。`config_migration_version` 非 0 却没给回调（或版本不认识）会被注册拒绝。
2. 记录读取拆成两步：`component_tail::readStored()` 不管适配、读出记录自己的 schema/长度/字节
   （并验证它自己的校验和），`decode()` 才是"按当前定义严格匹配"。这样"读得出来但用不上"和
   "记录坏了"不再混为一谈。
3. `native_logic::restoreTailConfig()` 在 schema 或长度不符时调用
   `migrateTailConfig()`：只有 `TC_COMPONENT_CONFIG_MIGRATE_OK` 才把结果写进实例、并通过
   `publishTailConfig()` 把记录升级（下一次存档带走新 schema）。`KEEP`/`REJECT`/未知返回码都不
   写回，实例跑注册的默认配置，原字节保留。迁移不算一次写入（revision 不变）。
4. 触发条件刻意很窄：只有 `ForeignType` 之外的、**同一个定义 id** 的 schema/长度不符才迁移；
   校验和不符、格式号未知、超过 1024 字节预算的记录连回调都不给。
5. 真机夹具 `build/nl_not1legacy.data`：与 `nl_not1board.data` 同形同线，只是实例的 tail 里已经有
   一条 **schema 6 / 4 字节**、用宿主同一个 FNV-1a 校验过的记录。探针 `tests/component-storage-probe.cpp`
   用 `TC_STORAGE_MIGRATE=accept|reject` 选择回调行为，并报告迁移被调用的次数与它收到的字节。
   每次启动前清空 loader.log，因此每条日志断言只可能来自本次启动。

## 第二刀交付（2026-09-22）

1. 宿主把配置写进 `0x4e` 元件记录自己的键值表（游戏 `save_monger/versions/v7` 的
   `Table[int64, int64]`，记录内偏移 `+0x190`），游戏把它作为元件记录的一部分序列化进原理图。
   键空间：高 32 位 `TCM3` 魔数，低 32 位字段号——
   `1` 格式、`2` 定义 id、`3` schema<<32|长度、`4` 校验和、`0x1000+i` 第 i 个 8 字节分块。
   不认识的表项原样保留。
2. **绑定即回读**：`src/native_logic.hpp` 的 `restoreTailConfig()` 在实例绑定时解码记录，
   只有全部字段一致才覆盖 `default_config`，所以 `on_create` 看到的是存档值；解码失败只报告、
   不写回、不覆盖旧字节（迁移留给下一刀）。
3. **写入 = 提交记录**：`storageWriteConfig` 先由 `publishTailConfig()` 构造记录、用宿主自己的
   解码器复核，再按“分块 → 定义/格式/schema+长度 → 校验和最后”的顺序写，最后才改内存。
   记录写不进去时返回 `UNAVAILABLE` 且内存配置保持旧值。
4. **受控 wrapper**（`src/native.hpp`）：只在游戏线程、只在当前棋盘里按 `(custom_id,
   instance_id)` 精确匹配的记录上操作，每次调用重新解析记录，从不保存记录或表指针；
   读取只在已提交且可读的内存区域内、按 `0x1000` 上限遍历表槽。
5. **构建画像**：`compat/profiles.json` 新增 `layout` 段（`CUSTOM_TAIL_TABLE_OFFSET=0x190`、
   `CUSTOM_TAIL_ELEMENT_STRIDE=0x18`、`CUSTOM_TAIL_ELEMENT_HEADER=0x8`），
   `generate-compat.ps1` 校验并生成 `TC_LAYOUT_*`；未命中时 `bindTailStorageSources()` 不装配
   访问器，`info.flags` 不含 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE`。
6. **新符号别名** `save.custom_tail_set` =
   `X5BX5Deq___modelZsave95mongerZversionsZv7_u70`，调用形状 `(table*, key:i64, value:i64)`，
   与游戏自己的反序列化器（`get_component__modelZsave95mongerZversionsZv7_u5+0x4cb`）一致。
7. ABI：`info.flags` 新增 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE`（结构体布局不变）；
   基线已更新。

## 本轮交付

1. `TCComponentTypeDefinitionV2` 末尾追加 `config_schema/config_size/default_config`：宿主在注册时
   复制最多 64 KiB 的默认配置，每个实例再得到独立副本。
2. `TCLogicIOV2` 末尾追加只读 `config/config_size/config_schema`；老回调的前缀布局不变，新回调按
   `size` 探测尾字段。
3. 新服务 `tc.component.storage` V1（SDK：`sdk/tc_component_storage.h`）：
   - `info`：schema、配置/状态大小、配置 revision；
   - `read_config/write_config`：generation 校验、整 blob 原子替换；
   - `capture_state/restore_state`：同尺寸仿真状态快照与恢复。
4. RESET 语义固定：只清仿真状态并运行 RESET 回调，配置保持不变；配置更新后下一次回调看到新值。
5. 修正 `TCComponentTypeDefinitionV2` 的 size-prefix 兼容：注册不再要求 `size == sizeof(当前结构)`，
   而是只要求首发前缀，尾字段按调用方 `size` 探测。

## 尚未做（第三刀之后）

| 未做 | 原因 / 下一步证据 |
|---|---|
| 缺失 Mod 占位 | **已测量，未实现**。见下面第 6 条：丢失发生在加载期，且不在 `get_custom_prototype` 那条路径上 |
| `on_clone/on_load/on_save/on_config_changed` | 需要复制/载入/配置事务能被游戏侧准确区分 |
| Undo/Redo | 配置写入改的是元件存档数据，还没有形成一条原生撤销记录；见下面第 7 条 |
| 仿真状态是否入档 | `capture_state/restore_state` 仍只在内存里；要不要默认持久化是 PLAN §18 的决策点 |

## 存档布局（真机确认，第二刀已接入宿主）

2026-09-22 新增了隔离的 `component-persistence-playtest` 变体。夹具在一个 `0x4e` 自定义实例的
**custom tail** 中写入一对 64 位哨兵，然后在真实游戏中修改 value、调用游戏自己的 schematic
序列化入口、退出并重启。第二次启动读到了新值，且 `circuit.data` 的 SHA-256 相对初始夹具发生变化。

运行时组件记录中已确认：

```text
+0x188  u64     custom prototype id
+0x190  u64     custom tail table capacity（单条夹具实测为 0x40）
+0x198  pointer custom tail hash-table storage
+0x1a0  u64     live entry count（单条夹具实测为 1）
```

夹具的表项：

```text
key   = 0x54434d3343464701  // "TCM3CFG" + format byte
value = 0x013579bdf2468ace  // first launch
value = 0x02468ace13579bdf  // saved, then observed after restart
```

当前构建的表桶里 key/value 连续出现；地址会随进程变化，不能保存裸指针。序列化格式位于自定义实例
末尾：`u16 count`，随后每项两个 `i64`。`tests/and-component-netlist.js` 已能在调试模式下把普通
settings 键值打印出来，便于继续做保存文件差分。

表本身是 Nim 的 `Table[int64, int64]`：`+0x190` 是 data 序列长度（槽位数，2 的幂），`+0x198` 是
序列载荷，`+0x1a0` 是活条目数；元素 i 在 `载荷 + 8 + 24*i`，其中 `+0` 是哈希（0 = 空槽）、
`+8` 是键、`+0x10` 是值。写入用游戏自己的 `[]=`（`save.custom_tail_set`），读取按上面这组偏移
遍历。这两个结论都由反汇编 + 真机双向确认：

- `get_component__modelZsave95mongerZversionsZv7_u5` 的反序列化段先写 `+0x188`、读 `u16` 计数、
  再 `lea r14,[rbx+0x190]`，然后每对 `i64` 调一次 `X5BX5Deq___modelZsave95mongerZversionsZv7_u70`；
- `tests/component-persistence-playtest.ps1 -Mode insert` 从空表插入、扩容、存档、重启回读。

真机通过命令：

```powershell
.\tests\component-persistence-playtest.ps1
.\tests\component-persistence-playtest.ps1 -Mode insert
.\tests\component-storage-playtest.ps1
.\tests\component-storage-playtest.ps1 -Mode migrate
.\tests\component-storage-playtest.ps1 -Mode reject
```

最近一次证据（前三条是存取，后两条是迁移）：

```text
run 1: PASS insert inserted value=87109624524081870 count=49 slots=128
run 2: PASS insert readback 87109624524081870 then updated value=163971054138006495 count=49 slots=128
run 3: PASS insert readback 163971054138006495 stable value=163971054138006495 count=49 slots=128

run 1: PASS storage launch=1 default value=11223344 stored=aabbccdd revision=1->2 persistence=1
run 2: PASS storage launch=2 readback-first value=aabbccdd stored=01234567 revision=1->2 persistence=1
run 3: PASS storage launch=3 readback-second value=01234567 stored=01234567 revision=1->1 persistence=1

run 1: PASS storage launch=1 migrated value=a55a0ff0 migrations=1 legacy=1
run 2: PASS storage launch=2 upgraded-record value=a55a0ff0 migrations=0 legacy=0

run 1: PASS storage launch=1 refused value=11223344 migrations=1 legacy=1
run 2: PASS storage launch=2 refused-again value=11223344 migrations=1 legacy=1
```

测试调用 `save_this_schematic__modelZboardZschematics_u132`，因为测试驱动用底层 `load_level` 换板，
没有走正常 UI 的完整关卡/脏状态切换。`save_level_data`、`save_all_design_changes` 和
`save_level_design` 在这个人工上下文里仍可能无写盘；这不等于正常 UI 保存路径失效。

## 已否定的路线

不要把 M3 blob 塞进组件普通 `settings` 序列。保存文件中的普通 settings 是字符串键值对，并且由
原型的 setting 描述驱动；在没有对应描述的自定义元件上硬塞未知 setting 会让关卡载入卡住。该实验
已经回退，当前 storage 夹具只使用 custom tail 表。

也不要使用外置 sidecar 作为最终方案：它无法随复制/Undo/缺失 Mod 占位一起移动，不满足阶段 3
验收。sidecar 最多只能作为调试工具。

## 当前改动入口

- `tests/and-component-fixture.cpp`
  - `Writer::settings` 明确写普通 string/string settings；
  - `addV13CustomInstance` 可写 custom tail 的 `i64/i64` 表项；
  - `topology == "storage"` 生成专用存档夹具。
- `build.ps1`
  - 生成 `build/and2_component_storage.data`、`build/and2_solution_storage.data` 和全部
    `build/nl_*.data`；
  - 构建 `tests/component-persistence-probe.cpp`（桶扫描与 insert 两种模式）和
    `tests/component-storage-probe.cpp`（经服务读写配置）。
- `tests/component-persistence-probe.cpp`
  - `TC_PERSISTENCE_MODE=insert` 时只用游戏表赋值函数：空表插入 → 灌 48 键扩容 → 存档 →
    重启回读（三次启动）；
  - 默认模式仍保留旧桶扫描探针的行为，用于对照；
  - 调用 `save_this_schematic` 写回隔离 profile。
- `tests/component-persistence-playtest.ps1`
  - `-Mode inplace|insert`，分别两次/三次启动；
  - 按运行断言具体值（insert 模式断言 `inserted` / `readback ... updated` / `readback ... stable`）；
  - 断言目标 `circuit.data` 哈希相对初始夹具发生变化。
- `tests/component-storage-playtest.ps1` + `tests/component-storage-probe.cpp`
  - 注册一个带 4 字节配置（schema 7）的一进一出元件，载入 `build/nl_not1board.data`，
    显式编译（未编译的关卡不会有绑定）；
  - 只经 `tc.component.storage` 读写配置，用 `save_this_schematic` 存档，三次启动断言
    `default → aabbccdd → 01234567` 的回读链；
  - `-Mode migrate|reject` 换成夹具 `build/nl_not1legacy.data`（同板型，实例已带 schema 6 的记录）
    并设 `TC_STORAGE_MIGRATE=accept|reject`，两次启动分别断言"迁移通过并在下次启动无迁移读回"与
    "拒绝升级且下次启动又被喂到同一份老字节"；
  - 每次启动前清空 loader.log，再按启动断言对应的日志行（`has no stored configuration` /
    `restored 4 configuration byte(s)` / `migrated its stored configuration from schema 6 ...` /
    `refused to upgrade a stored configuration written under schema 6`）。
- `src/native.hpp` 的缺失 Mod 捕获：`armMissingModCapture()` / `observeTailSet()` /
  `beginMissingModCapture()` / `boardRecordOf()` / `reportMissingMods()`；窗口在 `level.load` 链节
  （调原函数前打开），报告在 `frame()` 里出。
- `tests/component-placeholder-playtest.ps1` + `tests/component-placeholder-probe.cpp`
  - 探针**不注册任何元件**，只报告棋盘：每个槽的 kind/坐标/旋转/id/custom id、custom tail 的槽数、
    活条目数与排序后的 `key=value`（`report.txt`；另外写出滤掉探针自己哨兵的 `baseline.txt`）；
  - `TC_PLACEHOLDER_SAVE=1` 决定是否调用 `save_this_schematic`；`TC_PLACEHOLDER_TRACE=1` 会钩
    `get_custom_prototype__modelZboardZcustom95prototype95list_u451`，打印加载期间每次原型查询的
    id、结果与调用者地址；
  - 剧本四个场景：只加载 / 装回 Mod / 缺 Mod 存档 / 先装过再移除；每次启动的 loader.log 另存为
    `<场景>.log` 一起作为证据。
- `tests/and-component-netlist.js`
  - 调试输出保留普通 settings，支持 `TC_NETLIST_DUMP_ONLY=1`。

`tests/component-persistence-probe.cpp` 仍是**布局探针**：它的桶扫描和原位写值只用于对照，
正式服务不做任何原位写；宿主只用 `save.custom_tail_set`，写入前先用自带解码器复核。

## 离线验收

`tests/native-component.cpp` 覆盖：

- 默认配置在 `on_create` 前已就绪；
- count-only/短缓冲/正常配置读取；
- schema 与长度不匹配拒绝；
- 配置成功写入后 revision 递增，下一次 RESET 回调读到新配置；
- RESET 清仿真状态但保留配置；
- 状态快照可恢复；
- 伪造/失效 generation 访问存储返回 `ERR_STALE`；
- 持久化：绑定即回读记录、写入后内存与记录一致、**相同值不碰记录**、记录写失败时内存保持旧值、
  别的定义写的记录既不被采用也不被覆盖（`FakeTail` 替身表驱动）；
- 迁移：`MIGRATE_OK` 时回调拿到的是电路里的老字节、实例与 `on_create` 看到转换后的值、记录被升级
  成新 schema；`KEEP`/`REJECT`/未知返回码都保持默认配置且记录字节一个都没变；没声明迁移的定义
  不会调用回调。

`tests/component-tail.cpp`（fast 层）覆盖记录编解码：0/1/4/7/8/9/17/1024 字节往返、提交顺序
（分块在前、校验和最后）、每一种拒绝理由、预算边界、不认识的表项原地保留，以及"记录与定义不符
时 `readStored()` 仍读得出它自己的 schema/长度/字节，而 `decode()` 报 `BadSchema`/`BadLength`"。

`tests/services.cpp` 覆盖服务表的版本、尺寸与五个函数入口；ABI 快照覆盖新增结构、尾字段、常量
（新增 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE` 与迁移的三个结果码；`TCComponentTypeDefinitionV2`
的 `sizeof` 136 → 160，全部是尾部追加）。

## 下一刀建议顺序

1. ~~游戏表赋值函数的独立真机测试~~（已完成，`component-persistence-playtest -Mode insert`）。
2. ~~build-profile 别名与受控 wrapper~~（已完成，`save.custom_tail_set` + `layout` 段 +
   `src/native.hpp` 的 `tailTable*` 三个入口）。
3. ~~冻结宿主记录键空间~~（已完成：`src/component_tail.hpp`，`TCM3` + 字段号，配置上限 1024 字节。
   可选状态分块与引脚快照**尚未**纳入，见下一条）。
4. ~~绑定时解码 + `write_config` 原子提交~~（已完成，`restoreTailConfig` / `publishTailConfig`）。
5. ~~新增 migration 回调与明确结果~~（已完成，`component-storage-playtest -Mode migrate|reject`）。
6. 做缺失 Mod 占位 —— **第四刀已经把它量清楚了**，实现从这里接：
   - 实测（`component-placeholder`，四个场景，见 [verification.md](verification.md)）：缺 Mod 时元件
     在**加载期**就被换成 kind 0 墓碑（`custom=0 tombstones=1`），连线保留但悬空；只加载不存档
     **不会**动文件；一旦在缺 Mod 时存档，元件与配置记录**永久消失**，装回 Mod 也找不回来；
     "以前装过 Mod"不构成保护（游戏不跨会话保留自定义原型）。
   - 已排除的修法：探针钩 `get_custom_prototype__modelZboardZcustom95prototype95list_u451`
     （`load_schematic_raw+0x855` 调它）后，整次加载里**没有任何一次查询针对夹具里的
     `0x4e4f54315f303031`**（只看到 `12dc0381356d9010` 两次，且该 id 不在夹具中）。所以
     "未知 id 返回占位原型"必须挂在真正决定丢弃的那段代码上。
   - **已经夹到了**（同日继续）：场景 0 对照证明"带 Mod 时同一次 dump 能看到 `kind=4e custom=...`、
     记录还是 schema 6"，缺 Mod 时同一时刻只有墓碑；再钩 tail 写入函数（解析器
     `get_component__..._v13_u3+0x7ae` 自己用它）证明**缺 Mod 时解析器照样把整条记录读出来了**
     （5 次 tail-set，键值逐字节相同），列表层也是无条件 `add`。结论：丢弃发生在
     **"解析完成 → 写进棋盘"之间**，而且不经原型查询 API（`custom95prototype95list` 的 hashmap
     只有本模块 5 个函数引用、没有内联点；这 5 个函数在装载路径上只被 `load_schematic_raw+0x855`
     调用一次，追踪显示与我们的 id 无关）。
   - 下一步（一次差分追踪）：同时钩 `update_uses__modelZboardZcustom95prototype_u780`、
     `update_custom_used_components__modelZboardZcustom95prototype_u2729`、
     `custom_prototypes_del__...u291`、`in_custom_prototypes__...u9`、`get_prototype__...u502`，
     比较"带 Mod / 缺 Mod"两次加载中哪个只在带 Mod 时被调用，那里就是挂占位的点。
   - **捕获与诊断已落地**（同日，先于那次追踪）：解析器把整条记录建在自己的栈上——v13 反序列化器
     kind 在 `[rsp+0xe0]`、table 在 `[rsp+0x270]`，正好相差记录内表偏移 `0x190`——所以钩住
     `save.custom_tail_set` 就能从 `table - 0x190` 读出**正在构造的整条记录**，缺 Mod 时同样拿得到
     （真机 trace：`record=kind=4e x=-5 y=0 rot=0 id=2222... custom=4e4f54315f303031` 加全部 tail
     键值）。运行时据此实现 `armMissingModCapture()`（启动时接管 setter）、
     `beginMissingModCapture()`（`level.load` 链节调原函数前开窗）、按实例 id 归档、下一帧
     `reportMissingMods()` 判定"该 id 没有注册定义 + 记录不在棋盘上"，并报告坐标/旋转/schema/字节数。
     诊断带对照：场景 0（元件有主）不出现该行，场景 1/3/4（缺 Mod）都出现。
   - **救援已落地**（同日第二段）：缺 Mod 时把捕获的记录写进
     `<loader 数据目录>/missing-mods/<关卡>.bin`（`TCM3RSQ1` + 记录数 + custom id / 实例 id /
     坐标 / 旋转 / 全部 tail 键值）；下次装载该关卡时，主人已注册且 (custom id, 坐标) 上没有元件的
     条目，用已有的放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918`
     （`board_edits::buildPlacement` 模板）放回棋盘，再逐项把 tail 通过游戏自己的 setter 写回，
     成功后从 store 移除。真机链路：缺 Mod 存档 → `kept 1 record(s)` → 装回 Mod →
     `Missing Mod rescue: put custom 0x... back at (-5,0) rotation 0 with 5 stored configuration
     entries` → `tc.component.storage` 探针找到实例并迁移（`migrated ... legacy=1`）→ `kept 0 record(s)`。
     用例原先断言"装回也找不回来"，现在断言"能恢复"。
   - **两点如实记录**：① 元件由游戏的放置助手新建，实例 id 会变（连线按坐标连，不受影响；插件句柄
     看到的是新实例）；② 救援一次性——注入后条目即从 store 移除。
   - **仍未做，且已决定并入 M5**（2026-09-22 与项目所有者确认）：缺 Mod 期间**画面上的占位元件**。
     现在游戏仍把元件变成墓碑，我们只在主人回来时放回，所以"显示诊断信息的占位元件"目前是**日志
     诊断**；要画面可见需要在记录坐标处画一个标记，而棋盘绘制本来就是 M5 §10.1 的内容（含"错误
     状态"），所以在 M5 里与其它绘制一起做，不在 M3 里单挂一个绘制钩子。若将来要做占位原型，必须
     在真 Mod 注册前撤掉，否则 `registerNativeComponentV2` 会以 `DUPLICATE` 拒绝。
   - 占位原型仍需能被真 Mod 注册时替换（`registerNativeComponentV2` 目前遇到同 id 会以
     `DUPLICATE` 拒绝，所以占位必须在注册前撤掉）。
7. ~~接原生 Undo 快照~~（**已完成**，见下）：入口是
   `add_undo_changes__modelZboardZboard_u23805`（实体在 `.part.0`），参数是**一个 seq**：
   `[rcx]`=元素数、`[rcx+8]`=数据指针，元素步长 **0x490**，元素 +8 处是 tag 字节，函数按它分发；
   元素本身是**变体对象**（清理路径对同一元素里十多个偏移调 `eqdestroy__modelZsave95mongerZversionsZv0_*`，
   每个 16 字节 = 一个 seq/string 头），所以撤销栈存的是**内容**而不是地址——这正是配置能跟着走的前提。
   它的 10 个调用者就是游戏的全部编辑操作（旋转/删除/改尺寸/放置/拖拽提交/剪贴板/导线注释等）。
   包装层还有一个提前返回：`board[campaign[loaded_level]]` 记录的 `+0x40` 为 6、7 或 0 时不登记撤销，
   测量时必须确认关卡状态不落在这三类，否则会误判。
   下一步（一次测量）：勾住 `add_undo_changes`，在 `component-placement` 已有的真机操作里 dump 每个 tag
   的元素前 0x60 字节，实测"组件变更"变体的布局；然后按同样形状为配置提交登记一条，再用
   `board.undo` / `board.redo` 断言 tail 表回到提交前的字节（`src/native.hpp` 已持有
   `save.custom_tail_set` 原函数指针，回填由游戏自己做）。
   **第一次 dump 已完成**：钩子装在 `tests/component-placement-probe.cpp` 的 `TC_UNDO_TRACE=1`
   分支里（用例照常通过），但实测发现参数比"裸 seq 头"多一层——`arg` 指向一个多成员结构
   （`+0=1`、`+8=指针`、`+0x10=0x40`、`+0x18=指针`、`+0x30=0x40`、`+0x38=指针`），`arg+8` 那个缓冲
   以 `1` 开头、后面全零，真正的 0x490 元素在再下一层间接上。下一刀换目标：钩
   `eqcopy__modelZboardZboard_u22985.part.0`（调用处 `rcx=dst` 栈缓冲、`rdx=src` 元素地址），
   直接 dump src 前 0x40 字节。dump 设施已在探针里，只需改一个地址。
   **第二次测量（改用行为问法）**：`eqcopy` 的 dump 同样是全零（元素在再下一层间接），所以换
   `tests/component-undo-probe.cpp`（新增，如实报告、不进 catalog）：写配置 A → 选元件并旋转
   （已知会登记撤销）→ 写配置 B → `board.undo`，结果 `undo=0 afterUndo=b1b2b3b4`，撤销没生效。
   同一次尝试暴露两个坑，**下一刀必须先绕开**：
   ① `board_delete_component__modelZboardZboard_u10711` 不登记撤销（底层删除；UI 的
   `delete__presenterZutilitiesZhelper95functions_u5932` 才调 `add_undo_changes`）；
   ② **不能通过 `tc.component.storage` 读撤销后的配置**——元件被删后绑定仍在（下一次编译才释放），
   服务读到的是宿主内存副本；必须直接读棋盘记录的 tail 表。
   下一刀：用 `component-placement` 已验证的**命令总线** `TC_COMMAND_BOARD_UNDO` 发撤销，读配置走
   棋盘记录，编辑入口改用 UI 那条路径。
   **第五刀已交付**（2026-09-22）：确认游戏自己的撤销栈**不表达配置变更**（变更种类都是棋盘编辑，
   写值的命令一条都不登记），于是宿主自己保存一步的**前后字节**，并在游戏撤销/重做入口被按下且还有
   未消费步骤时把它写回元件记录、报告"已处理"；栈空时原样交给游戏，棋盘编辑手感不变。上限 64 步、
   LIFO、新写入清空重做栈、实例释放或换板即丢弃相关步骤。真机证据（`component-undo`，15.4s 通过）：

```text
PASS one undo restored the first configuration and one redo re-applied the second:
     instance=2459565876494606882 wrote=a1a2a3a4 then=b1b2b3b4 afterUndo=a1a2a3a4 afterRedo=b1b2b3b4
```

   同一次测量排除了两个坑（务必记住）：① 游戏 Board API 的第一参数是 `load_level` 那个对象，
   不是 `+0x78` 的表；② 判据要读**棋盘记录里的表**，不能读 `tc.component.storage`（绑定会保留
   内存副本）。
8. M2 回补：~~`on_config_changed`~~、~~`on_clone`~~、~~`on_load`~~、~~`on_save`~~（**均已交付**，见下），仅剩 move/rotate/resize 与 compile 类回调（这两类计划排在最后，价值主要是缓存失效与棋盘级簿记）。
   - `on_clone`（同日）：复制作为宿主自己的编辑操作（`TC_COMMAND_BOARD_DUPLICATE_COMPONENT`），一次 Ctrl+Z 撤掉整次复制；`on_clone` 在 `on_create` 之后触发、配置已就位。
   - `on_load`/`on_save`（同日）：前者在"记录里的配置真正装上"时触发（被拒绝的记录不发），后者在游戏写出电路前派发给每个活实例（`save.level` + 新别名 `save.schematic`），回调里可以提交派生数据/修复配置，正在写的文件就看到它。两个回调的锁纪律：`on_save` 在实例锁外运行，别在 `on_load`/`on_clone` 里调服务。
   - `on_config_changed`（2026-09-22，本项第一刀）：`TCComponentLifecycleV1` 末尾追加该回调，
     `TCLogicPhase` 加 `TC_LOGIC_CONFIG_CHANGED = 5`；一次成功的 `write_config` 与一次撤销/重做各触发
     回调在**实例锁之外**运行，因此可以在里面调用 `tc.component.storage`——第一版把通知留在锁内，
     离线用例立刻死锁，改成"锁内改状态、锁外通知"才过。证据：`tests/native-component.cpp` 的
     触发/不触发/回调内调服务/记录与内存一致四组断言；真机 `component-storage` 三个模式在 PASS 行
     报 `config_changes=`，playtest 分别断言 persist `1/1/0`、migrate 与 reject `0/0`。
     ABI 基线新增 `TC_LOGIC_CONFIG_CHANGED` 与 `offsetof(TCComponentLifecycleV1, on_config_changed)`
     （结构 24 → 32，尾部追加）。

另外两件第二刀明确留下的尾巴：

- 状态分块：现在只把**配置**写进记录，仿真状态仍是内存快照（`capture_state/restore_state`）。
  要不要默认持久化状态是 §18 的决策点之一，先别偷偷写进去。
- 上限压力：1024 字节 = 4 个头部项 + 128 个分块项。表扩容在 64→128 槽上已验证，但没有验证过
  大量实例 × 大配置同时存档时的膨胀，调整上限前先补这个测量。

## M3 收口清单（2026-09-22）

| 验收（计划 §9） | 证据 |
|---|---|
| 存储契约：配置序列化/反序列化、大小查询、配额错误、失败不覆盖原数据 | 键值表记录（`TCM3` + FNV-1a）；`component-storage`（36.9s）三个模式；`component-tail`（离线） |
| 缺失和升级：保留未知 blob、装回 Mod 后恢复 | `component-placeholder`（74.5s）四个场景 + 救援注入；`component-storage-migrate`（25.7s）/`-reject`（26.1s） |
| 编辑事务：配置变更进撤销系统、批量工具可显式分组 | `component-undo`（15.8s）：一次写入一步、`begin_edit`/`commit_edit` 一组一步、redo 恢复；`tc.component.storage` V2 |
| 上限与配额 | `component-capacity`：1024 字节 = 132 表项，跨重启一致，约 1 KB/实例 |

**明确留给后续的**（不阻塞 M3 收口，各自有归属）：

- 缺 Mod 期间**画面上**的占位元件 → M5 §10.1 的棋盘绘制（经项目所有者同意并入）。
- 仿真状态是否默认入档 → 计划 §18 的决策点，现在 `capture_state/restore_state` 仍只在内存。
- 复制（`on_clone`）、`on_load/on_save`、move/rotate/resize 与编译类回调 → M2 回补的剩余部分。
- 把一次配置提交与棋盘编辑合成同一组撤销 → `tc.transactions` 与配置栈对接（现在两者各自独立）。

接手时如果只做一件事，建议先跑 `tools\test.ps1 -Tier game` 里这几条：`component-storage`、
`component-storage-migrate`、`component-storage-migrate-reject`、`component-placeholder`、
`component-undo`、`component-capacity`——它们一起覆盖 M3 的全部真机验收。

## 接手时的纪律

- 工作树包含大量同一计划的未提交改动；不要 reset、checkout 或覆盖无关文件。
- 先运行上面的专用真机用例，再改正式服务；普通 settings 卡载入的失败实验不要重复。
- 任何新 ABI 都要同步 SDK C 头、C++ wrapper、服务查询、离线测试、ABI 快照、真机证据、SDK 文档、
  limits、verification、示例和 changelog；只有测试探针不需要更新 ABI 基线。
- 当前 build profile 是 `tc-win64-2.1.334`。custom tail 偏移和内部表赋值符号都必须由 profile 控制，
  未命中构建时 storage 持久化返回 unavailable，不能猜布局。
