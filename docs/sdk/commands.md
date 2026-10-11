# 命令总线

读状态走 Board 快照；改状态走 `TC_SERVICE_COMMANDS`。命令总线把 Mod 的意图先排队，在本帧所有
插件回调结束后由 Loader 统一校验和执行，避免插件各自直接调用游戏内部函数、在别人的回调中
重入游戏，或用已经失效的对象指针修改新场景。

## 查询与提交

```cpp
TCCommandApiV1 commands{};
if (tc::commandService(host, &commands) != TC_SERVICE_OK) return 2;

TCCommandV1 stop{
    sizeof(TCCommandV1), TC_COMMAND_SIM_STOP, 0, 0,
    board_handle, 0
};
uint64_t request = 0;
if (commands.submit(commands.context, &stop, &request) != TC_COMMAND_OK) {
    // 参数、线程、容量或 Board 生命周期错误；没有入队。
}
```

V1 的首批命令是：

| type | 行为 | argument |
|---|---|---|
| `TC_COMMAND_SIM_RUN` | 运行到目标周期 | 非负目标周期 |
| `TC_COMMAND_SIM_STOP` | 停止／刷新 | 忽略 |
| `TC_COMMAND_SIM_RESET` | 重置仿真 | 忽略 |
| `TC_COMMAND_BOARD_UNDO` | 调用游戏原生 Board undo | 忽略 |
| `TC_COMMAND_BOARD_REDO` | 调用游戏原生 Board redo | 忽略 |
| `TC_COMMAND_SAVE` | 把当前 Board 写入本存档中**游戏自己会读的那份**电路：`schematics/<关卡 kind>/Default/circuit.data`（kind 取自 `campaign/<关卡目录>/meta.txt` 的 `kind = …`，因为游戏读写的是 kind 目录而不是关卡名，见 `docs/sdk/simulation.md`） | 忽略 |

这六种命令都要求 `subject` 是当前有效的 Board 句柄。`flags` 必须为 0；未知 type、伪造 kind、
失效 Board 会在入队前拒绝。

## 异步状态

`submit` 成功只表示进入队列，不等于游戏已经执行。Mod 在后续 `on_frame` 查询结果：

```cpp
TCCommandStatusV1 status{};
if (commands.get_status(commands.context, request, &status, sizeof(status)) == TC_COMMAND_OK) {
    if (status.state == TC_COMMAND_STATE_SUCCEEDED) {
        // status.result == TC_COMMAND_OK
    }
}
```

状态单向变化：`QUEUED → RUNNING → SUCCEEDED/FAILED`；排队期间可以 `cancel`，变成
`CANCELLED`，开始执行后取消返回 `TC_COMMAND_ERR_STATE`。结果按提交 Mod 隔离，另一个 Mod 即使
猜到 request id 也只能得到 `TC_COMMAND_ERR_NOT_FOUND`。Loader 有界保留最近 512 条记录；已经
完成且很旧的 id 最终会变成 NOT_FOUND。

## 时序与边界

- `submit`、`get_status`、`cancel` 都只允许在游戏主/渲染线程调用；V1 不从仿真线程事件直接改游戏。
- 同一 Mod 最多有 64 条未完成命令；达到上限返回 `TC_COMMAND_ERR_CAPACITY`。
- 执行前再次验证 Board 句柄。排队后若玩家离开或换关，结果为 `FAILED/STALE`，不会把旧请求施加
  到新 Board。
- 插件加载失败或被拒时，其排队命令统一取消。
- 执行通过 Loader 的稳定 `sim.do` 别名，因此现有 hook chain 与 `TC_EVENT_SIM_COMMAND` 仍能观察
  同一条路径；命令总线没有绕开共存机制。

Undo/Redo 使用当前构建真实导出的 presenter 入口；反汇编核对其唯一参数就是 Board 模型，返回
false（没有可撤销/重做项）会成为 `FAILED/TC_COMMAND_ERR_STATE`。Save 走游戏原生
`save.schematic` 写入当前电路（不是只保存关卡进度的 `save.level`），仍会先派发 `TC_EVENT_SAVE`
与元件的 `on_save` 生命周期。后续 Board 编辑命令会继续扩充 type，并在事务层组合；不会要求
Mod 回退到裸函数调用。

## V2：带载荷的命令（元件放置）

V1 的命令只有 `type` 与 `argument`，够表达运行／停止／撤销／保存，但表达不了"在某个格点放一个
元件"。`TCCommandApiV2` 保留 V1 前缀，追加放置载荷；V1 表照旧可查。

V2 目前有三种编辑命令，都用同一个 Board 句柄做 `subject`：

| type | 行为 | 载荷 | 详见 |
|---|---|---|---|
| `TC_COMMAND_BOARD_PLACE_COMPONENT` | 在格点放一个元件 | `custom_prototype_id`／`kind`／`x`／`y`／`rotation` | 本节 |
| `TC_COMMAND_BOARD_DUPLICATE_COMPONENT` | 复制一个自定义实例（含配置，一次 Ctrl+Z 撤掉整个复制） | `argument` = 源实例 id、`custom_prototype_id` = 源类型、`x`／`y`／`rotation` = 目的地 | [services.md](services.md) 的 `on_clone` 一节 |
| `TC_COMMAND_BOARD_DELETE_COMPONENT` | 删掉一个自定义实例（可撤销） | `argument` = 实例 id、`custom_prototype_id` = 类型 | 下文的"V2：删除元件" |

```cpp
TCCommandApiV2 commands{};
if (tc::commandService(host, &commands) != TC_SERVICE_OK) return 2;

TCCommandV2 place{};
place.size = sizeof(place);
place.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
place.subject = board;                        // 当前 Board 句柄
place.custom_prototype_id = my_component_id;  // 自定义元件；0 表示改用内置 kind
place.kind = 0x4e;                            // 自定义实例的 kind
place.x = 20;
place.y = 6;
place.rotation = 0;
uint64_t request = 0;
if (commands.submit(commands.context, &place, &request) != TC_COMMAND_OK) return 3;
```

**为什么一个 Board 句柄就够**：游戏菜单的 add 助手
（`add_component__presenterZutilitiesZhelper95functions_u5918`）第一个参数就是 Board model，
而句柄正好解析到它——所以放置不需要 presenter 上下文，也不需要 Mod 自己做 UI 自动化。加载器在
启动时解析这个符号，解析不到时放置命令返回 `TC_COMMAND_ERR_UNAVAILABLE`。

**Undo/Redo 只回退一步，不是回退一个事务。** 用一个两步事务（连续放两个元件）实测：一次
`TC_COMMAND_BOARD_UNDO` 只让**最后一步**消失，另一步还在。所以"事务 = 一步撤销"不成立；想让
玩家的 Ctrl+Z 一次抹掉整批，Mod 得自己按步数发 undo，或者记录并执行自己的逆操作。

**被撤销的元件不会缩短序列，而是变成"墓碑"。** 撤销后元件的原始序列长度不变，被撤掉的槽位变成
一条 kind 为 0 的全零记录（实测：`[0x00(0,0)] [0x4e(30,0)] [0x4e(20,6)] [0x4e(24,10)] [0x00(0,0)]`）。
因此**数元件要按 kind 跳过 0**，`component_count` 只是序列长度。`TC_COMMAND_BOARD_REDO` 会把
那条记录还原。

入队前的校验：`subject` 必须仍是有效 Board；`custom_prototype_id` 非零时 `kind` 必须是 `0x4e`，
否则 `kind` 必须非零（内置 kind）；坐标必须放得进记录的 16 位域；`rotation` ≤ 255。助手返回
false 时结果是 `FAILED/TC_COMMAND_ERR_STATE`。

事务可以承载它：`TCTransactionApiV2.stage` 接受 `TCCommandV2`，于是"放几个元件 + 存档"是一次带
冲突预检的提交（见 [transactions.md](transactions.md)）。

真机证据（`component-placement-playtest`，与 V3/V4/V5 共用同一个沙箱）：

```text
Board edits: component placement armed
component placement command submit=0 request=1 before=2
component placement command complete state=3 result=0 submitted=295 completed=295
component placement command board before=2 after=3 found=1
```

`found=1` 是按公开路径回读的：V3 枚举 → V4 读数，命中的元件 kind `0x4e`、自定义 id 与坐标
`(20,6)` 全部匹配——命令确实改变了棋盘，而不只是返回成功。

### V2：删除元件（可撤销）

```cpp
TCCommandV2 erase{};
erase.size = sizeof(erase);
erase.type = TC_COMMAND_BOARD_DELETE_COMPONENT;
erase.subject = board;                    // 当前 Board 句柄
erase.custom_prototype_id = myTypeId;     // 目标元件的类型
erase.argument = instanceToDelete;        // 目标元件的实例 id（V4 读到的 id）
uint64_t request = 0;
if (commands.submit(commands.context, &erase, &request) != TC_COMMAND_OK) return 3;
```

载荷和复制命令同一套身份（实例 id + 类型）：`argument` 是实例 id，`custom_prototype_id` 是它
所属的类型，`x/y/rotation/kind` 不使用。命令按两句名字找目标——序列里 kind `0x4e`
（`board_edits::kCustomKind`）、`+8` 处的实例 id、`+0x188` 处的自定义 id 全部匹配的那条记录——
找不到就是 `FAILED/TC_COMMAND_ERR_NOT_FOUND`，不删任何东西。

**删除走游戏自己的撤销感知包装**（`try_delete__modelZutilities_u8585`），不是低层
`board_delete_component`：它先复制要删的记录、连在元件上的导线一起删，最后把变更列表交给游戏的
撤销栈。所以一条 `TC_COMMAND_BOARD_UNDO` 会把元件（含其导线）放回来，一条
`TC_COMMAND_BOARD_REDO` 再把它去掉——这正是低层墓碑删除做不到的（墓碑没有撤销条目，见
[research/board-object-fields.md](../research/board-object-fields.md) §6.1）。撤销后槽位仍然留在
序列里，只是变成 kind 0 记录，所以数元件同样要跳过 kind 0。

**别人的类型删不掉。** 入队前会查元件目录（`tc::component_registry`）里这个类型登记的
`owner_mod`：如果目录把它记在**另一个** Mod 名下，命令直接 `TC_COMMAND_ERR_ARGUMENT`（未入队，
`request` 保持 0），棋盘一个字节都不会动。目录只记加载器经手过的注册——声明式
`tc.component.types` 和由加载器桥接的电路导入都带 `owner_mod`。

**目录没有记录的类型则不做归属判断**：一个 Mod 用 `tc.component.model` 自己导入电路
（`examples/circuit-and`、`examples/custom-or` 走的就是这条路），这条路完全在加载器视野之外，目录
里没有它的条目，加载器无从判断归属，于是命令照原样执行——和 `DUPLICATE_COMPONENT` 一样，
总线在这条路上不承担"这是谁的元件"的判断。真机证据里两条都有：删另一个 Mod 的类型被拒（`-2`，
棋盘不变），删一个没人注册过的类型 id 则入队后在棋盘回读处失败（`NOT_FOUND`）。

真机证据（同一个 `component-placement-playtest`，在放置与事务相位之后）：

```text
component delete command submit=0 request=4 instance=…
component delete command state=3 result=0 live=3->2 at(28,10)=0
component delete undo state=3 result=0 live=3 at(28,10)=1
component delete redo state=3 result=0 live=2 at(28,10)=0
component delete peer placement state=3 result=0 instance=… at(12,10)=1
component delete foreign type submit=-2 request=0 still=1
component delete unknown type state=4 result=-5 at(12,10)=1
```

第三行是这条命令与低层删除的分界：撤销把元件放回同一格，`at(28,10)` 又从 0 变回 1；重做再清掉。
随后三行是归属规则的两半：`submit=-2` 说明另一个 Mod 的类型连队都没进（`request=0`，`still=1`
说明棋盘上的元件还在），而没人注册过的类型 id 会正常入队、在棋盘回读处以 `NOT_FOUND` 失败。
peer 用的是本仓库自带的 `local.editor-demo` 包，它的类型 id 是公开的。

**导线编辑故意还没有进总线**：这个构建里唯一"不需要上下文"的加线入口
（`add_wire_from_pos`）只是鼠标按下式的起步——它留下的记录两个端点相同、状态槽为奇数，而且游戏
自己的 `get_wire(point)` 拒绝寻址它。所以 `PLACE_WIRE` 不进契约，理由与实验数据在
[research/board-object-fields.md](../research/board-object-fields.md) §4。
